/*
 *      Copyright (C) 2015-2019 Team KODI
 *      http:/kodi.tv
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "WebBrowserClient.h"

#include "addon.h"
#include "MessageIds.h"
#include "URICheckHandler.h"
#include "Utils.h"
#include "gui/DialogBrowserContextMenu.h"
#include "interface/Handler.h"
#include "interface/JSDialogHandler.h"
#include "utils/StringUtils.h"
#include "utils/SystemTranslator.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_parser.h"
#include "include/wrapper/cef_helpers.h"
#include "include/base/cef_bind.h"
#include "include/views/cef_textfield.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <kodi/ActionIDs.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/XBMC_vkeys.h>
#include <kodi/gui/dialogs/ContextMenu.h>
#include <kodi/gui/dialogs/FileBrowser.h>
#include <kodi/gui/dialogs/Keyboard.h>
#include <kodi/gui/dialogs/YesNo.h>
#include <stdio.h>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

#define DEBUG_LOGS

#define ZOOM_MULTIPLY 25.0

CWebBrowserClient::CWebBrowserClient(KODI_HANDLE handle, int uniqueClientId, const std::string& startURL, CWebBrowser* instance)
  : CWebControl(handle, uniqueClientId),
    m_mainBrowserHandler{instance},
    m_renderViewReady{false},
    m_uniqueClientId{uniqueClientId},
    m_iMousePreviousFlags{0},
    m_iMousePreviousControl{MBT_LEFT},
    m_browserId{-1},
    m_browser{nullptr},
    m_browserCount{0},
    m_isFullScreen{false},
    m_isLoading{false},
    m_v8Kodi(this)
{
  m_fMouseXScaleFactor = GetWidth() / GetSkinWidth();
  m_fMouseYScaleFactor = GetHeight() / GetSkinHeight();

  LOG_MESSAGE(ADDON_LOG_DEBUG, "Current browser client sizes:");
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Mouse X Scale Factor: %f", m_fMouseYScaleFactor);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Mouse Y Scale Factor: %f", m_fMouseYScaleFactor);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - X Position:           %f", GetXPos());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Width:                %f", GetWidth());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Skin X Position:      %f", GetSkinXPos());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Skin Width:           %f", GetSkinWidth());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Y Position:           %f", GetYPos());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Height:               %f", GetHeight());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Skin Y Position:      %f", GetSkinYPos());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - Skin Height:          %f", GetSkinHeight());

  // CEF related sub classes to manage web parts
  m_resourceManager = new CefResourceManager();

  // Create the browser-side router for query handling.
  m_jsDialogHandler = new CJSDialogHandler(this);
  m_renderer = new CRendererClient(this);
  m_audioHandler = new CAudioHandler();
  m_dialogContextMenu = new CBrowerDialogContextMenu(this);
}

CWebBrowserClient::~CWebBrowserClient()
{
}

bool CWebBrowserClient::SetActive()
{
  m_renderViewReady = true;
  if (m_browser.get())
  {
    m_browser->GetHost()->SetFocus(true);
    SetOpenedAddress(m_currentURL);
    SetOpenedTitle(m_currentTitle);
    SetIconURL(m_currentIcon);
    return true;
  }

  return false;
}

bool CWebBrowserClient::SetInactive()
{
  m_renderViewReady = false;

  if (m_browser.get())
  {
    m_browser->GetHost()->SetFocus(false);
    return true;
  }

  return false;
}

bool CWebBrowserClient::CloseComplete()
{
  if (m_browser.get())
  {
    m_browser->GetHost()->CloseBrowser(true);
    m_browser = nullptr;
    return true;
  }

  return false;
}

void CWebBrowserClient::DestroyRenderer()
{
  m_renderer = nullptr;
}

void CWebBrowserClient::SendKey(int key)
{
  CefRefPtr<CefBrowserHost> host = m_browser->GetHost();
  CefKeyEvent key_event;
  key_event.windows_key_code = key;
  key_event.type = KEYEVENT_RAWKEYDOWN;
  host->SendKeyEvent(key_event);
  key_event.type = KEYEVENT_CHAR;
  host->SendKeyEvent(key_event);
  key_event.type = KEYEVENT_KEYUP;
  host->SendKeyEvent(key_event);
}

bool CWebBrowserClient::HandleScrollEvent(int actionId)
{
  switch (actionId)
  {
    case ACTION_MOVE_LEFT:
      SendKey(VKEY_LEFT);
      break;
    case ACTION_MOVE_RIGHT:
      SendKey(VKEY_RIGHT);
      break;
    case ACTION_MOVE_UP:
      SendKey(VKEY_UP);
      break;
    case ACTION_MOVE_DOWN:
      SendKey(VKEY_DOWN);
      break;
    case ACTION_PAGE_UP:
      SendKey(VKEY_PRIOR);
      break;
    case ACTION_PAGE_DOWN:
      SendKey(VKEY_NEXT);
      break;
  }

  double scrollOffsetX = m_renderer->ScrollOffsetX();
  double scrollOffsetY = m_renderer->ScrollOffsetY();
  if (scrollOffsetX == m_scrollOffsetX &&
      scrollOffsetY == m_scrollOffsetY)
  {
    return false;
  }

  m_scrollOffsetX = scrollOffsetX;
  m_scrollOffsetY = scrollOffsetY;
  return true;
}

bool CWebBrowserClient::OnAction(int actionId, uint32_t buttoncode, wchar_t unicode, int &nextItem)
{
  if (!m_browser.get())
    return false;

fprintf(stderr, "--> %s %i %i %i %i\n", __PRETTY_FUNCTION__, actionId, buttoncode, unicode, nextItem);

  CefRefPtr<CefBrowserHost> host = m_browser->GetHost();
  if (!m_focusOnEditableField)
  {
    int deltaX = 0;
    int deltaY = 0;

    switch (actionId)
    {
      case ACTION_VOLUME_UP:
      case ACTION_VOLUME_DOWN:
      case ACTION_VOLAMP:
      case ACTION_MUTE:
        return false;
      case ACTION_NAV_BACK:
      case ACTION_MENU:
      case ACTION_PREVIOUS_MENU:
        return false;
      case ACTION_MOVE_LEFT:
      case ACTION_MOVE_RIGHT:
      case ACTION_MOVE_UP:
      case ACTION_MOVE_DOWN:
      case ACTION_PAGE_UP:
      case ACTION_PAGE_DOWN:
      {
        if (!HandleScrollEvent(actionId))
        {
          if      (actionId == ACTION_MOVE_LEFT)  nextItem = GetGUIItemLeft();
          else if (actionId == ACTION_MOVE_RIGHT) nextItem = GetGUIItemRight();
          else if (actionId == ACTION_MOVE_UP)    nextItem = GetGUIItemTop();
          else if (actionId == ACTION_MOVE_DOWN)  nextItem = GetGUIItemBottom();
          else if (actionId == ACTION_PAGE_UP)    nextItem = GetGUIItemTop();
          else if (actionId == ACTION_PAGE_DOWN)  nextItem = GetGUIItemBottom();
          return false;
        }
        break;
      }
      case ACTION_FIRST_PAGE:
        SendKey(VKEY_HOME);
        return true;
      case ACTION_LAST_PAGE:
        SendKey(VKEY_END);
        return true;
      case ACTION_ZOOM_OUT:
      {
        int zoomTo = kodi::GetSettingInt("main.zoomlevel") - kodi::GetSettingInt("main.zoom_step_size");
        if (zoomTo < 30)
          break;

        LOG_MESSAGE(ADDON_LOG_DEBUG, "%s - Zoom out to %i %%", __FUNCTION__, zoomTo);
        m_browser->GetHost()->SetZoomLevel(PercentageToZoomLevel(zoomTo));
        kodi::SetSettingInt("main.zoomlevel", zoomTo);
        break;
      }
      case ACTION_ZOOM_IN:
      {
        int zoomTo = kodi::GetSettingInt("main.zoomlevel") + kodi::GetSettingInt("main.zoom_step_size");
        if (zoomTo > 330)
          break;

        LOG_MESSAGE(ADDON_LOG_DEBUG, "%s - Zoom in to %i %% - %i %i", __FUNCTION__, zoomTo, kodi::GetSettingInt("main.zoomlevel"), kodi::GetSettingInt("main.zoom_step_size"));
        m_browser->GetHost()->SetZoomLevel(PercentageToZoomLevel(zoomTo));
        kodi::SetSettingInt("main.zoomlevel", zoomTo);
        break;
      }
      default:
        return false;
    };
    return true;
  }

  CefKeyEvent key_event;
  key_event.modifiers = CSystemTranslator::ButtonCodeToModifier(buttoncode);
  key_event.windows_key_code = CSystemTranslator::ButtonCodeToKeyboardCode(buttoncode);
  key_event.native_key_code = 0;
  key_event.is_system_key = false;
  key_event.character = unicode;
  key_event.unmodified_character = CSystemTranslator::ButtonCodeToUnmodifiedCharacter(buttoncode);
  key_event.focus_on_editable_field = m_focusOnEditableField;

  if (key_event.windows_key_code == VKEY_RETURN)
  {
    // We need to treat the enter key as a key press of character \r.  This
    // is apparently just how webkit handles it and what it expects.
    key_event.unmodified_character = '\r';
  }

  key_event.type = KEYEVENT_RAWKEYDOWN;
  host->SendKeyEvent(key_event);
  key_event.type = KEYEVENT_CHAR;
  host->SendKeyEvent(key_event);
  key_event.type = KEYEVENT_KEYUP;
  host->SendKeyEvent(key_event);

  return true;
}

bool CWebBrowserClient::OnMouseEvent(int id, double x, double y, double offsetX, double offsetY, int state)
{
  if (!m_browser.get())
    return true;

  static const int scrollbarPixelsPerTick = 40;

  CefRefPtr<CefBrowserHost> host = m_browser->GetHost();

  CefMouseEvent mouse_event;
  mouse_event.x = (x - GetSkinXPos()) * m_fMouseXScaleFactor;
  mouse_event.y = (y - GetSkinYPos()) * m_fMouseYScaleFactor;

  switch (id)
  {
    case ACTION_MOUSE_LEFT_CLICK:
    {
      mouse_event.modifiers = 0;
      mouse_event.modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
      host->SendMouseClickEvent(mouse_event, MBT_LEFT, false, 1);
      host->SendMouseClickEvent(mouse_event, MBT_LEFT, true, 1);
      m_iMousePreviousFlags = mouse_event.modifiers;
      m_iMousePreviousControl = MBT_LEFT;
      break;
    }
    case ACTION_MOUSE_RIGHT_CLICK:
      mouse_event.modifiers = 0;
      mouse_event.modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
      host->SendMouseClickEvent(mouse_event, MBT_RIGHT, false, 1);
      host->SendMouseClickEvent(mouse_event, MBT_RIGHT, true, 1);
      m_iMousePreviousFlags = mouse_event.modifiers;
      m_iMousePreviousControl = MBT_RIGHT;
      break;
    case ACTION_MOUSE_MIDDLE_CLICK:
      mouse_event.modifiers = 0;
      mouse_event.modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
      host->SendMouseClickEvent(mouse_event, MBT_MIDDLE, false, 1);
      host->SendMouseClickEvent(mouse_event, MBT_MIDDLE, true, 1);
      m_iMousePreviousFlags = mouse_event.modifiers;
      m_iMousePreviousControl = MBT_MIDDLE;
      break;
    case ACTION_MOUSE_DOUBLE_CLICK:
      mouse_event.modifiers = m_iMousePreviousFlags;
      host->SendMouseClickEvent(mouse_event, m_iMousePreviousControl, false, 1);
      host->SendMouseClickEvent(mouse_event, m_iMousePreviousControl, true, 1);
      m_iMousePreviousControl = MBT_LEFT;
      m_iMousePreviousFlags = 0;
      break;
    case ACTION_MOUSE_WHEEL_UP:
      host->SendMouseWheelEvent(mouse_event, 0, scrollbarPixelsPerTick);
      break;
    case ACTION_MOUSE_WHEEL_DOWN:
      host->SendMouseWheelEvent(mouse_event, 0, -scrollbarPixelsPerTick);
      break;
    case ACTION_MOUSE_DRAG:

      break;
    case ACTION_MOUSE_MOVE:
    {
      bool mouse_leave = state == 3 ? true : false;
      host->SendMouseMoveEvent(mouse_event, mouse_leave);
      break;
    }
    case ACTION_MOUSE_LONG_CLICK:

      break;
    default:
      break;
  }

  return true;
}

bool CWebBrowserClient::Initialize()
{
  if (!m_browser.get())
  {
    LOG_MESSAGE(ADDON_LOG_ERROR, "%s - Called without present browser", __FUNCTION__);
    return false;
  }

  m_browser->GetHost()->SetZoomLevel(PercentageToZoomLevel(kodi::GetSettingInt("main.zoomlevel")));

  return true;
}

void CWebBrowserClient::Render()
{
  if (m_renderViewReady)
    m_renderer->Render();
}

bool CWebBrowserClient::Dirty()
{
  if (!m_renderViewReady)
    return false;

  return m_renderer->Dirty();
}

bool CWebBrowserClient::OpenWebsite(const std::string& url)
{
  LOG_MESSAGE(ADDON_LOG_DEBUG, "Open website '%s'", url.c_str());
  if (!m_browser.get())
  {
    LOG_MESSAGE(ADDON_LOG_ERROR, "%s - Called without present browser", __FUNCTION__);
    return false;
  }

  CefRefPtr<CefFrame> frame = m_browser->GetMainFrame();
  if (!frame.get())
  {
    LOG_MESSAGE(ADDON_LOG_ERROR, "%s - Called without present frame", __FUNCTION__);
    return false;
  }

  std::string usedURL;
  if (url.empty())
    usedURL = kodi::GetSettingString("main.starturl");
  else
    usedURL = url;

  if (m_strStartupURL.empty())
    m_strStartupURL = usedURL;

  m_currentIcon = "DefaultFile.png"; // Use default image from Kodi itself
  SetIconURL(m_currentIcon);

  frame->LoadURL(usedURL);

  return true;
}

void CWebBrowserClient::OpenOwnContextMenu()
{
  std::vector<std::string> entries;
  entries.push_back(kodi::GetLocalizedString(30009));
  entries.push_back(kodi::GetLocalizedString(30323));
  entries.push_back(kodi::GetLocalizedString(30032));
  entries.push_back(kodi::GetLocalizedString(30324));

  int ret = kodi::gui::dialogs::ContextMenu::Show("", entries);
  if (ret >= 0)
  {
    switch (ret)
    {
      case 0:
      {
        m_mainBrowserHandler->GetGUIManager().GetDownloadDialog()->Open();
        break;
      }
      case 1:
      {
        m_mainBrowserHandler->GetGUIManager().GetCookieDialog().Open();
        break;
      }
      case 2:
      {
        kodi::OpenSettings();
        break;
      }
      case 3:
      {
        if (!m_currentURL.empty())
          kodi::SetSettingString("main.starturl", m_currentURL);
        break;
      }
      default:
        break;
    }
  }
}

bool CWebBrowserClient::GetHistory(std::vector<std::string>& historyWebsiteNames, bool behindCurrent)
{
  if (!m_browser)
    return false;

  bool currentFound = false;
  for (const auto& entry : m_historyWebsiteNames)
  {
    if (!behindCurrent && entry.second)
      break;
    if (entry.second)
    {
      currentFound = true;
      continue;
    }

    if (behindCurrent && !currentFound)
      continue;

    historyWebsiteNames.push_back(entry.first);
  }

  return true;
}

void CWebBrowserClient::SearchText(const std::string& text, bool forward, bool matchCase, bool findNext)
{
  if (m_browser)
  {
    if (m_currentSearchText != text)
      m_browser->GetHost()->StopFinding(true);
    m_browser->GetHost()->Find(0, text, forward, matchCase, findNext);
    m_currentSearchText = text;
  }
}

void CWebBrowserClient::StopSearch(bool clearSelection)
{
  if (m_browser)
    m_browser->GetHost()->StopFinding(clearSelection);
  m_currentSearchText.clear();
}

void CWebBrowserClient::ScreenSizeChange(float x, float y, float width, float height, bool fullscreen)
{
  fprintf(stderr, "--> %s %f %f %f %f %i\n", __PRETTY_FUNCTION__, x, y, width, height, fullscreen);
  m_isFullScreen = true;
  m_renderer->ScreenSizeChange(x, y, width, height);
  if (m_browser.get())
    m_browser->GetHost()->WasResized();
}

float CWebBrowserClient::GetWidth() const
{
  return kodi::addon::CWebControl::GetWidth();
}

float CWebBrowserClient::GetHeight() const
{
  return kodi::addon::CWebControl::GetHeight();
}

// -----------------------------------------------------------------------------

CefRefPtr<CefAudioHandler> CWebBrowserClient::GetAudioHandler()
{
  return m_audioHandler;
}

CefRefPtr<CefContextMenuHandler> CWebBrowserClient::GetContextMenuHandler()
{
  return m_dialogContextMenu;
}

CefRefPtr<CefDialogHandler> CWebBrowserClient::GetDialogHandler()
{
  return GetMain().GetGUIManager().GetFileDialog();
}

CefRefPtr<CefDownloadHandler> CWebBrowserClient::GetDownloadHandler()
{
  return GetMain().GetGUIManager().GetDownloadDialog();
}

CefRefPtr<CefJSDialogHandler> CWebBrowserClient::GetJSDialogHandler()
{
  return m_jsDialogHandler;
}

CefRefPtr<CefRenderHandler> CWebBrowserClient::GetRenderHandler()
{
  return m_renderer;
}




/// CefClient methods
//@{
bool CWebBrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefProcessId source_process,
                                                 CefRefPtr<CefProcessMessage> message)
{
  CEF_REQUIRE_UI_THREAD();

  if (m_messageRouter->OnProcessMessageReceived(browser, source_process, message))
    return true;

  std::string message_name = message->GetName();
  CefRefPtr<CefBrowserHost> host = browser->GetHost();
  if (message_name == RendererMessage::FocusedNodeChanged)
  {
    // A message is sent from ClientRenderDelegate to tell us whether the
    // currently focused DOM node is editable. Use of |focus_on_editable_field_|
    // is redundant with CefKeyEvent.focus_on_editable_field in OnPreKeyEvent
    // but is useful for demonstration purposes.
    m_focusOnEditableField = message->GetArgumentList()->GetBool(0);
    return true;
  }
  else if (message_name == RendererMessage::V8AddonCall)
  {
    m_v8Kodi.OnProcessMessageReceived(browser, source_process, message);
    return true;
  }

  return false;
}
//@}

/// CefDisplayHandler methods
//@{
void CWebBrowserClient::OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& url)
{
  CEF_REQUIRE_UI_THREAD();

  if (frame->IsMain())
  {
    m_currentURL = url.ToString();
    SetOpenedAddress(m_currentURL);
  }
}

void CWebBrowserClient::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title)
{
  CEF_REQUIRE_UI_THREAD();

  if (m_currentTitle != title.ToString())
  {
    m_currentTitle = title.ToString();
    SetOpenedTitle(m_currentTitle);
  }
}

void CWebBrowserClient::OnFaviconURLChange(CefRefPtr<CefBrowser> browser, const std::vector<CefString>& icon_urls)
{
  CEF_REQUIRE_UI_THREAD();

  unsigned int listSize = icon_urls.size();
#ifdef DEBUG_LOGS
  LOG_INTERNAL_MESSAGE(ADDON_LOG_DEBUG, "From currently opened web site given icon urls (first one used)");
  for (unsigned int i = 0; i < listSize; ++i)
    LOG_INTERNAL_MESSAGE(ADDON_LOG_DEBUG, " - Icon %i - %s", i+1, icon_urls[i].ToString().c_str());
#endif

  if (listSize > 0)
    m_currentIcon = icon_urls[0].ToString();
  else
    m_currentIcon = "";

  SetIconURL(m_currentIcon);
}

void CWebBrowserClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen)
{
  CEF_REQUIRE_UI_THREAD();

  if (m_isFullScreen != fullscreen)
  {
    m_isFullScreen = fullscreen;

    LOG_INTERNAL_MESSAGE(ADDON_LOG_DEBUG, "From currently opened web site becomes fullsreen requested as '%s'", m_isFullScreen ? "yes" : "no");
    SetFullscreen(m_isFullScreen);
  }
}

bool CWebBrowserClient::OnTooltip(CefRefPtr<CefBrowser> browser, CefString& text)
{
  CEF_REQUIRE_UI_THREAD();

  if (m_currentTooltip != text.ToString().c_str())
  {
    m_currentTooltip = text.ToString().c_str();
    SetTooltip(m_currentTooltip);
  }

  return true;
}

void CWebBrowserClient::OnStatusMessage(CefRefPtr<CefBrowser> browser, const CefString& value)
{
  CEF_REQUIRE_UI_THREAD();

  if (m_currentStatusMsg != value.ToString().c_str())
  {
    m_currentStatusMsg = value.ToString().c_str();
    SetStatusMessage(m_currentStatusMsg);
  }
}

bool CWebBrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line)
{
  LOG_INTERNAL_MESSAGE(ADDON_LOG_ERROR, "%s - Message: %s - Source: %s - Line: %i", __FUNCTION__,
                       message.ToString().c_str(), source.ToString().c_str(), line);
  return true;
}
//@}

/// CefLifeSpanHandler methods
//@{
bool CWebBrowserClient::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefFrame> frame,
                                      const CefString& target_url,
                                      const CefString& target_frame_name,
                                      CefRequestHandler::WindowOpenDisposition target_disposition,
                                      bool user_gesture,
                                      const CefPopupFeatures& popupFeatures,
                                      CefWindowInfo& windowInfo,
                                      CefRefPtr<CefClient>& client,
                                      CefBrowserSettings& settings,
                                      bool* no_javascript_access)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
// #ifdef DEBUG_LOGS
  LOG_MESSAGE(ADDON_LOG_DEBUG, "--------------------------------------->%s - %s", __FUNCTION__, std::string(target_url).c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - target_url '%s'", std::string(target_url).c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - target_frame_name '%s'", std::string(target_frame_name).c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - user_gesture '%i'", user_gesture);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - no_javascript_access '%i'", *no_javascript_access);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - windowInfo.x '%i'", windowInfo.x);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - windowInfo.y '%i'", windowInfo.y);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - windowInfo.height '%i'", windowInfo.height);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - windowInfo.width '%i'", windowInfo.width);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - windowInfo.windowless_rendering_enabled '%i'", windowInfo.windowless_rendering_enabled);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.height '%i'", popupFeatures.height);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.width '%i'", popupFeatures.width);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.heightSet '%i'", popupFeatures.heightSet);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.widthSet '%i'", popupFeatures.widthSet);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.menuBarVisible '%i'", popupFeatures.menuBarVisible);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.scrollbarsVisible '%i'", popupFeatures.scrollbarsVisible);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.statusBarVisible '%i'", popupFeatures.statusBarVisible);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - popupFeatures.toolBarVisible '%i'", popupFeatures.toolBarVisible);
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - target_disposition '%i'", target_disposition);
// #endif

  if (target_disposition == WOD_UNKNOWN)
  {
    kodi::Log(ADDON_LOG_ERROR, "Browser popup requested with unknown target disposition");
    return false;
  }

  windowInfo.windowless_rendering_enabled = true;

  if (!m_isFullScreen && kodi::GetSettingBoolean("main.allow_open_to_tabs") && target_disposition != WOD_CURRENT_TAB)
    RequestOpenSiteInNewTab(target_url); /* Request to do on kodi itself */
  else
    OpenWebsite(std::string(target_url));
  return false; /* Cancel popups in off-screen rendering mode */
}

void CWebBrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  m_browserCount++;

  if (m_messageRouter == nullptr)
  {
    // Create the browser-side router for query handling.
    CefMessageRouterConfig config;
    m_messageRouter = CefMessageRouterBrowserSide::Create(config);

    // Register handlers with the router.
    CreateMessageHandlers(m_messageHandlers);
    for (const auto& entry : m_messageHandlers)
      m_messageRouter->AddHandler(entry, false);
  }

  // Disable mouse cursor change if requested via the settings
  if (kodi::GetSettingBoolean("system.mouse_cursor_change_disabled"))
    browser->GetHost()->SetMouseCursorChangeDisabled(true);

  if (!m_browser.get())
  {
    m_browser = browser;
    m_browserId = browser->GetIdentifier();

    /* Inform Kodi the control is ready */
    SetControlReady(true);
  }
}

bool CWebBrowserClient::DoClose(CefRefPtr<CefBrowser> browser)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
  CEF_REQUIRE_UI_THREAD();
  return false; /* Allow the close. For windowed browsers this will result in the OS close event being sent */
}

void CWebBrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
  CEF_REQUIRE_UI_THREAD();

  m_messageRouter->OnBeforeClose(browser);
  if (--m_browserCount == 0)
  {
    for (const auto& entry : m_messageHandlers)
      m_messageRouter->RemoveHandler(entry);
    m_messageHandlers.clear();
    m_messageRouter = nullptr;
  }
}
//@}

/// CefRequestHandler methods
//@{
bool CWebBrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool user_gesture, bool is_redirect)
{
  CEF_REQUIRE_UI_THREAD();

  m_messageRouter->OnBeforeBrowse(browser, frame);
  return false;
}

bool CWebBrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString& target_url,
                                         CefRequestHandler::WindowOpenDisposition target_disposition, bool user_gesture)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
  return false;
}

CefRequestHandler::ReturnValue CWebBrowserClient::OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                                                       CefRefPtr<CefRequest> request, CefRefPtr<CefRequestCallback> callback)
{
  CEF_REQUIRE_IO_THREAD();

#ifdef DEBUG_LOGS
  LOG_MESSAGE(ADDON_LOG_DEBUG, "OnBeforeResourceLoad:");
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - url = '%s'", request->GetURL().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - method = '%s'", request->GetMethod().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - flags = %i", request->GetFlags());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - resourceType = %i", static_cast<int>(request->GetResourceType()));
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - transitionType = %i", static_cast<int>(request->GetTransitionType()));
#endif

  return m_resourceManager->OnBeforeResourceLoad(browser, frame, request, callback);
}

CefRefPtr<CefResourceHandler> CWebBrowserClient::GetResourceHandler(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                                                    CefRefPtr<CefRequest> request)
{
  CEF_REQUIRE_IO_THREAD();

  return m_resourceManager->GetResourceHandler(browser, frame, request);
  return nullptr;
}

void CWebBrowserClient::OnResourceRedirect(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                           CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response, CefString& new_url)
{
  CEF_REQUIRE_IO_THREAD();

#ifdef DEBUG_LOGS
  LOG_MESSAGE(ADDON_LOG_DEBUG, "On Resource Redirect:");
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - status = %i", response->GetStatus());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - statusText = '%s'", response->GetStatusText().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - contentType = '%s'", response->GetMimeType().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - url = '%s'", request->GetURL().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - id = %lu", request->GetIdentifier());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - new_url = '%s'", new_url.ToString().c_str());
#endif
}

bool CWebBrowserClient::OnResourceResponse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                           CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response)
{
  CEF_REQUIRE_IO_THREAD();

#ifdef DEBUG_LOGS
  LOG_MESSAGE(ADDON_LOG_DEBUG, "On Resource Response:");
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - status      = %i", response->GetStatus());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - statusText  = '%s'", response->GetStatusText().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - contentType = '%s'", response->GetMimeType().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - url         = '%s'", request->GetURL().ToString().c_str());
  LOG_MESSAGE(ADDON_LOG_DEBUG, " - id          = %lu", request->GetIdentifier());
#endif
  return false;
}

bool CWebBrowserClient::GetAuthCredentials(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, bool isProxy, const CefString& host,
                                           int port, const CefString& realm, const CefString& scheme, CefRefPtr<CefAuthCallback> callback)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
  ///TODO Useful and secure?
  return false;
}

bool CWebBrowserClient::OnQuotaRequest(CefRefPtr<CefBrowser> browser, const CefString& origin_url, int64 new_size, CefRefPtr<CefRequestCallback> callback)
{
  fprintf(stderr, "--> %s\n", __PRETTY_FUNCTION__);
  CEF_REQUIRE_IO_THREAD();

  static const int64 max_size = 1024 * 1024 * 20;  // 20mb.
  if (new_size > max_size)
    kodi::Log(ADDON_LOG_DEBUG, "JavaScript on '%s' requests a specific storage quota with size %li MBytes who becomes not granded",
                                origin_url.ToString().c_str(), new_size/1024/1024);

  // Grant the quota request if the size is reasonable.
  callback->Continue(new_size <= max_size);
  return true;
}

void CWebBrowserClient::OnProtocolExecution(CefRefPtr<CefBrowser> browser, const CefString& url, bool& allow_os_execution)
{
  CEF_REQUIRE_UI_THREAD();

  std::string urlStr = url;

  // Allow OS execution of Spotify URIs.
  if (urlStr.find("spotify:") == 0)
    allow_os_execution = true;
}

bool CWebBrowserClient::OnCertificateError(CefRefPtr<CefBrowser> browser, ErrorCode cert_error,
                                           const CefString& request_url, CefRefPtr<CefSSLInfo> ssl_info,
                                           CefRefPtr<CefRequestCallback> callback)
{
  CEF_REQUIRE_UI_THREAD();

  CefRefPtr<CefX509Certificate> cert = ssl_info->GetX509Certificate();
  if (cert.get())
  {
    bool canceled = false;
    std::string subject = cert->GetSubject()->GetDisplayName().ToString();
    std::string certStatusString = CURICheck::GetCertStatusString(ssl_info->GetCertStatus());
    std::string text = StringUtils::Format(kodi::GetLocalizedString(31001).c_str(), subject.c_str(), certStatusString.c_str(), subject.c_str());
    bool ret = kodi::gui::dialogs::YesNo::ShowAndGetInput(kodi::GetLocalizedString(31000), text, canceled,
                                                          kodi::GetLocalizedString(31003), kodi::GetLocalizedString(31002));

#ifdef SHOW_ERROR_PAGE
    if (!ret)
    {
      //Load the error page.
      LoadErrorPage(browser->GetMainFrame(), request_url, cert_error, GetCertificateInformation(cert, ssl_info->GetCertStatus()));
    }
#endif
    callback->Continue(ret);
    return true;
  }

  return false;  // Cancel the request.
}

void CWebBrowserClient::OnPluginCrashed(CefRefPtr<CefBrowser> browser, const CefString& plugin_path)
{
  kodi::Log(ADDON_LOG_ERROR, "Browser Plugin '%s' crashed (URL: '%s'", plugin_path.ToString().c_str(),
                                                                       browser->GetFocusedFrame()->GetURL().ToString().c_str());
}

void CWebBrowserClient::OnRenderViewReady(CefRefPtr<CefBrowser> browser)
{
  m_renderViewReady = true;
}

void CWebBrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status)
{
  LOG_MESSAGE(ADDON_LOG_DEBUG, "%s", __FUNCTION__);
  CEF_REQUIRE_UI_THREAD();

  m_messageRouter->OnRenderProcessTerminated(browser);

  // Don't reload if there's no start URL, or if the crash URL was specified.
  if (m_strStartupURL.empty() || m_strStartupURL == "chrome://crash")
    return;

  CefRefPtr<CefFrame> frame = browser->GetMainFrame();
  std::string url = frame->GetURL();

  // Don't reload if the termination occurred before any URL had successfully
  // loaded.
  if (url.empty())
    return;

  std::string start_url = m_strStartupURL;
  StringUtils::ToLower(url);
  StringUtils::ToLower(start_url);

  // Don't reload the URL that just resulted in termination.
  if (url.find(start_url) == 0)
    return;

  frame->LoadURL(m_strStartupURL);
}
//@}

/// CefFindHandler methods
//@{
void CWebBrowserClient::OnFindResult(CefRefPtr<CefBrowser> browser, int identifier, int count, const CefRect& selectionRect,
                                     int activeMatchOrdinal, bool finalUpdate)
{
  if (finalUpdate && activeMatchOrdinal <= 1)
  {
    std::string text = StringUtils::Format(kodi::GetLocalizedString(30038).c_str(), count, m_currentSearchText.c_str());
    kodi::QueueNotification(QUEUE_INFO, kodi::GetLocalizedString(30037), text);
  }
  LOG_MESSAGE(ADDON_LOG_DEBUG, "%s -identifier %i, count %i finalUpdate %i activeMatchOrdinal %i", __FUNCTION__, identifier, count, finalUpdate, activeMatchOrdinal);
}
//@}




/// CefLoadHandler methods
//@{
void CWebBrowserClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward)
{
  CEF_REQUIRE_UI_THREAD();

  SetLoadingState(isLoading, canGoBack, canGoForward);
}


void CWebBrowserClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type)
{
  LOG_MESSAGE(ADDON_LOG_DEBUG, "Load started (id='%d', URL='%s'", browser->GetIdentifier(), frame->GetURL().ToString().c_str());
  CEF_REQUIRE_UI_THREAD();

  m_isLoading = true;
  Initialize();
}

void CWebBrowserClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
  LOG_MESSAGE(ADDON_LOG_DEBUG, "Load done with status code '%i'", httpStatusCode);
  CEF_REQUIRE_UI_THREAD();
  m_isLoading = false;

  class CHistoryReporter : public CefNavigationEntryVisitor
  {
  public:
    CHistoryReporter(std::vector<std::pair<std::string, bool>>& historyWebsiteNames) : m_historyWebsiteNames(historyWebsiteNames)
    {
      m_historyWebsiteNames.clear();
    }
    virtual bool Visit(CefRefPtr<CefNavigationEntry> entry, bool current, int index, int total) override
    {
      m_historyWebsiteNames.push_back(std::pair<std::string, bool>(entry->GetTitle(), current));
      return true;
    }

  private:
    std::vector<std::pair<std::string, bool>>& m_historyWebsiteNames;
    IMPLEMENT_REFCOUNTING(CHistoryReporter);
  };
  browser->GetHost()->GetNavigationEntries(new CHistoryReporter(m_historyWebsiteNames), false);
}

void CWebBrowserClient::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                                    const CefString& errorText, const CefString& failedUrl)
{
  CEF_REQUIRE_UI_THREAD();

  // Don't display an error for downloaded files.
  if (errorCode == ERR_ABORTED)
    return;

  // Don't display an error for external protocols that we allow the OS to
  // handle. See OnProtocolExecution().
  if (errorCode == ERR_UNKNOWN_URL_SCHEME)
  {
    std::string url = frame->GetURL();
    if (url.find("spotify:") == 0)
      return;
  }

  kodi::QueueNotification(QUEUE_WARNING, kodi::GetLocalizedString(30133), failedUrl.ToString());

  // Load the error page.
  CURICheck::LoadErrorPage(frame, failedUrl, errorCode, errorText);
}
//@}

int CWebBrowserClient::ZoomLevelToPercentage(double zoomlevel)
{
  return static_cast<int>((zoomlevel*ZOOM_MULTIPLY)+100.0);
}

double CWebBrowserClient::PercentageToZoomLevel(int percent)
{
  return (static_cast<double>(percent-100))/ZOOM_MULTIPLY;
}

void CWebBrowserClient::CreateMessageHandlers(MessageHandlerSet& handlers)
{
  handlers.insert(new CJSHandler(this));
}