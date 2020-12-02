/*
 *  Copyright (C) 2015-2020 Alwin Esch (Team Kodi) <https://kodi.tv>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// Own
#include "../WebBrowserClient.h"

// CEF
#include "include/cef_browser.h"

// Dev-kit
#include "../../../../lib/kodi-dev-kit/include/kodi/gui/Window.h"

enum KEYBOARD
{
  CAPS,
  LOWER,
  SYMBOLS
};

namespace chromium
{
namespace app
{
namespace main
{
namespace gui
{

class CBrowserDialogKeyboard : public kodi::gui::CWindow
{
public:
  CBrowserDialogKeyboard();

  void Show(CefRefPtr<CWebBrowserClient> client, cef_text_input_mode_t input_mode);

  bool OnInit() override;
  bool OnAction(const kodi::gui::input::CAction& action) override;
  bool OnClick(int controlId) override;

private:
  void UpdateButtons();
  void Backspace();
  void OnOK();
  void OnSymbols();
  void OnShift();
  void Character(const std::string& ch);
  void NormalCharacter(const std::string& ch);
  void OnLayout();
  void MoveCursor(int amount);

  CefRefPtr<CWebBrowserClient> m_client{nullptr};
  cef_text_input_mode_t m_input_mode{CEF_TEXT_INPUT_MODE_DEFAULT};
  int m_keyboardPos{0};
  std::string m_currentLayoutName;
  std::string m_strHeading;
  KEYBOARD m_keyType{LOWER};
  bool m_bShift{false};
};

} /* namespace gui */
} /* namespace main */
} /* namespace app */
} /* namespace chromium */