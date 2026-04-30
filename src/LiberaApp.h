#pragma once

struct GLFWwindow;
struct ImFont;

struct LiberaAppConfig {
    const char* title = "Libera Timecode";
    int width = 720;
    int height = 540;
    int windowX = -1;
    int windowY = -1;
};

struct LiberaApp {
    GLFWwindow* window = nullptr;
    float dpiScale = 1.0f;
    ImFont* fontBase = nullptr;
    ImFont* fontMedium = nullptr;
    ImFont* fontLarge = nullptr;
    ImFont* fontReadout = nullptr;

    bool init(const LiberaAppConfig& config);
    bool beginFrame();
    void endFrame();
    void shutdown();
    void getWindowGeometry(int& x, int& y, int& w, int& h) const;
};
