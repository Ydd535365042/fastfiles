#pragma once

// Application icon (res/FastFiles.ico, drawn by tools/make-icon.ps1); the lowest id, so Explorer shows it for the exe
#define IDI_APP 1

// Sidebar section icons from Files (Assets/FluentIcons/SidebarSections, MIT): id = base + section * 4 + scale index
// (scale index 0..3 = 100%, 125%, 150%, 200%)
#define IDR_SECTION_ICON_BASE 1000

// UI translations (res/lang.txt from tools/gen-lang.ps1), see Lang.cpp
#define IDR_LANG 900
