#include "MainWindow.h"
#include "Lang.h"
#include "ContentDialog.h"

// The status center (StatusCenter.cpp, Files' StatusCenter): shared by every long-running file operation (copy,
// move, delete, recycle, empty the bin). FastFiles has no built-in compress / extract (removed at the user's
// request: the shell's own menu items - WinRAR, 7-Zip... - cover it), so this file only keeps the generic plumbing.

void MainWindow::UpdateActivity()
{
    m_center.Refresh();
}

void MainWindow::OnOperationDone(unsigned id)
{
    Operations::View v{};
    if (!m_ops.Find(id, v))
        return;
    // Files keeps the result card (Success / Failed / Cancelled) until it's dismissed
    m_center.Refresh();
    SetStatusText(StatusCenter::Header(v), true);
}

// WM_CLOSE with jobs still running: ask, then cancel them. Going to the tray doesn't stop them, so no question then.
bool MainWindow::ConfirmCloseWithOperations()
{
    const int running = m_ops.Running();
    if (running == 0 || (m_primary && m_settings.background && !m_quitting))
        return true;
    // Files' UWP shell keeps running in the background on close, so it has no equivalent prompt; FastFiles keeps its own text.
    // MB_DEFBUTTON2 in the old MessageBox: Enter must not cancel the running operations, so close stays default
    const std::wstring text = std::to_wstring(running) + Tr(L" 个文件操作还没完成。关闭会取消它们。\n\n仍然关闭吗？");
    if (!ContentDialog::Confirm(m_hwnd, &m_theme, L"FastFiles", text, Tr(L"是"), Tr(L"取消"), true))
    {
        m_quitting = false;
        return false;
    }
    m_ops.CancelAll();
    // Give the workers a moment to stop and clean up their partial output (the shell's copy engine stops at its
    // next progress callback)
    for (int i = 0; i < 60 && m_ops.Running() > 0; ++i)
        Sleep(50);
    m_center.Hide();
    return true;
}
