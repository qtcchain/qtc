# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

function(generate_setup_nsi)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  set(abs_top_builddir ${PROJECT_BINARY_DIR})
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  # Keep the bitcoin: URI scheme until the Qt/payment handling surface is
  # migrated separately from this build-label cleanup.
  set(CLIENT_TARNAME "bitcoin")
  set(QTC_GUI_NAME "qtc-qt")
  set(QTC_DAEMON_NAME "qtcd")
  set(QTC_CLI_NAME "qtc-cli")
  set(QTC_TX_NAME "qtc-tx")
  set(QTC_WALLET_TOOL_NAME "qtc-wallet")
  set(QTC_TEST_NAME "test_qtc")
  set(EXEEXT ${CMAKE_EXECUTABLE_SUFFIX})
  configure_file(${PROJECT_SOURCE_DIR}/share/setup.nsi.in ${PROJECT_BINARY_DIR}/qtc-win64-setup.nsi USE_SOURCE_PERMISSIONS @ONLY)
endfunction()
