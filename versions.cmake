# This maintains the links for all sources used by this superbuild.
# Simply update this file to change the revision.
# One can use different revision on different platforms.
# e.g.
# if (UNIX)
#   ..
# else (APPLE)
#   ..
# endif()

set(CEF_COMPLETE_VERSION 2357.1274.g7b49af6)
if(CEF_COMPLETE)
  set(CEF_BRANCH_VERSION 2357)
  set(CEF_KODI_CHANGE_PATCH "0001-CEF-2357-Add-Kodi-related-changes-Version-0.0.1.patch")

  # CEF_OS_NAME and BITSIZE are defined by CMakeLists.txt
  add_revision(cef-binary
  )
else()
  # CEF_OS_NAME and BITSIZE are defined by CMakeLists.txt
  add_revision(cef-binary
    URL http://esmasol.de/data/documents/cef_binary_3_${CEF_COMPLETE_VERSION}_${CEF_OS_NAME}${BITSIZE}.zip
    URL_MD5 a4bd865ad8f3093ece22176c4accfcdd
  )
endif()
