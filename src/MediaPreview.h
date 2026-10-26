#pragma once
#include "Theme.h"

// Files' MediaPreview (MediaPlayerElement with compact transport controls, not playing until asked): Media Foundation's
// MFPlay renders video into a child window; under it a self-drawn bar with play/pause, the position, the time and mute.
// The window text says the state ("已暂停 0:00 / 0:03"), for screen readers and tests.
class MediaPreview
{
public:
    ~MediaPreview();
    bool Create(HWND parent, const Theme* theme);
    void Load(const std::wstring& path); // stops whatever played before
    void Unload();
    void Move(const RECT& rc);
    void Show(bool show);
    HWND Hwnd() const { return m_hwnd; }

private:
    struct Callback;
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void OnEvent(int type, HRESULT hr);
    void Paint(HDC hdc, const RECT& rc);
    void Layout();
    void TogglePlay();
    void UpdateTitle();
    RECT BarRect() const;
    RECT PlayRect() const;
    RECT TrackRect() const;
    RECT MuteRect() const;
    ULONGLONG Position() const; // 100 ns
    int Hit(POINT pt) const;

    const Theme* m_theme = nullptr;
    HWND m_hwnd = nullptr, m_video = nullptr;
    struct IMFPMediaPlayer* m_player = nullptr;
    Callback* m_callback = nullptr;
    std::wstring m_path;
    ULONGLONG m_duration = 0;
    bool m_hasVideo = false, m_playing = false, m_muted = false, m_ready = false, m_failed = false, m_seeking = false;
    int m_hot = 0;
};
