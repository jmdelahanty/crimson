#pragma once

#include <string>

void processHelpMenuShortcut(bool& show_help_window);
void drawHelpMenuWindow(bool show_help_window);
void drawErrorPopup(bool& show_error, const std::string& error_message);
