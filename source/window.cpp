#include "window.hpp"

#include <hex.hpp>
#include <hex/api/content_registry.hpp>

#include <iostream>
#include <numeric>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_freetype.h>
#include <imgui_imhex_extensions.h>

#include "helpers/plugin_handler.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace hex {

    constexpr auto MenuBarItems = { "File", "Edit", "View", "Help" };

    void *ImHexSettingsHandler_ReadOpenFn(ImGuiContext *ctx, ImGuiSettingsHandler *, const char *) {
        return ctx; // Unused, but the return value has to be non-null
    }

    void ImHexSettingsHandler_ReadLine(ImGuiContext*, ImGuiSettingsHandler *handler, void *, const char* line) {
        for (auto &view : ContentRegistry::Views::getEntries()) {
            std::string format = std::string(view->getName()) + "=%d";
            sscanf(line, format.c_str(), &view->getWindowOpenState());
        }
    }

    void ImHexSettingsHandler_WriteAll(ImGuiContext* ctx, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf) {
        buf->reserve(buf->size() + 0x20); // Ballpark reserve

        buf->appendf("[%s][General]\n", handler->TypeName);

        for (auto &view : ContentRegistry::Views::getEntries()) {
            buf->appendf("%s=%d\n", view->getName().data(), view->getWindowOpenState());
        }

        buf->append("\n");
    }

    Window::Window(int &argc, char **&argv) {
        hex::SharedData::mainArgc = argc;
        hex::SharedData::mainArgv = argv;

        this->initGLFW();
        this->initImGui();

        ContentRegistry::Settings::add("Interface", "Color theme", 0, [](nlohmann::json &setting) {
            static int selection = setting;
            if (ImGui::Combo("##nolabel", &selection, "Dark\0Light\0Classic\0")) {
                setting = selection;
                return true;
            }

            return false;
        });

        ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        EventManager::subscribe(Events::SettingsChanged, this, [](auto) -> std::any {
            int theme = ContentRegistry::Settings::getSettingsData()["Interface"]["Color theme"];
            switch (theme) {
                default:
                case 0: /* Dark theme */
                    ImGui::StyleColorsDark();
                    break;
                case 1: /* Light theme */
                    ImGui::StyleColorsLight();
                    break;
                case 2: /* Classic theme */
                    ImGui::StyleColorsClassic();
                    break;
            }
            ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg] = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

            return { };
        });

        EventManager::subscribe(Events::FileLoaded, this, [this](auto userData) -> std::any {
            auto path = std::any_cast<std::string>(userData);

            this->m_recentFiles.push_front(path);

            {
                std::list<std::string> uniques;
                for (auto &file : this->m_recentFiles) {

                    bool exists = false;
                    for (auto &unique : uniques) {
                        if (file == unique)
                            exists = true;
                    }

                    if (!exists)
                        uniques.push_back(file);

                    if (uniques.size() > 5)
                        break;
                }
                this->m_recentFiles = uniques;
            }

            {
                std::vector<std::string> recentFilesVector;
                std::copy(this->m_recentFiles.begin(), this->m_recentFiles.end(), std::back_inserter(recentFilesVector));

                ContentRegistry::Settings::write("ImHex", "RecentFiles", recentFilesVector);
            }

            return { };
        });

        EventManager::subscribe(Events::CloseImHex, this, [this](auto) -> std::any {
            glfwSetWindowShouldClose(this->m_window, true);

            return { };
        });

        ContentRegistry::Settings::load();
        View::postEvent(Events::SettingsChanged);

        for (const auto &path : ContentRegistry::Settings::read("ImHex", "RecentFiles"))
            this->m_recentFiles.push_back(path);
    }

    Window::~Window() {
        this->deinitImGui();
        this->deinitGLFW();
        ContentRegistry::Settings::store();

        for (auto &view : ContentRegistry::Views::getEntries())
            delete view;
        ContentRegistry::Views::getEntries().clear();

        this->deinitPlugins();

        EventManager::unsubscribe(Events::SettingsChanged, this);
    }

    void Window::loop() {
        while (!glfwWindowShouldClose(this->m_window)) {
            this->frameBegin();

            for (const auto &call : View::getDeferedCalls())
                call();
            View::getDeferedCalls().clear();

            for (auto &view : ContentRegistry::Views::getEntries()) {
                if (!view->isAvailable() || !view->getWindowOpenState())
                    continue;

                auto minSize = view->getMinSize();
                minSize.x *= this->m_globalScale;
                minSize.y *= this->m_globalScale;

                ImGui::SetNextWindowSizeConstraints(minSize, view->getMaxSize());
                view->drawContent();
            }

            View::drawCommonInterfaces();

            #ifdef DEBUG
                if (this->m_demoWindowOpen)
                    ImGui::ShowDemoWindow(&this->m_demoWindowOpen);
            #endif

            this->frameEnd();
        }
    }

    bool Window::setFont(const std::filesystem::path &path) {
        if (!std::filesystem::exists(path))
            return false;

        auto &io = ImGui::GetIO();

        // If we have a custom font, then rescaling is unnecessary and will make it blurry
        io.FontGlobalScale = 1.0f;

        // Load font data & build atlas
        std::uint8_t *px;
        int w, h;
        io.Fonts->AddFontFromFileTTF(path.string().c_str(), std::floor(14.0f * this->m_fontScale)); // Needs conversion to char for Windows
        ImGuiFreeType::BuildFontAtlas(io.Fonts, ImGuiFreeType::Monochrome);
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

        // Create new font atlas
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA8, GL_UNSIGNED_INT, px);
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(tex));

        return true;
    }

    void Window::frameBegin() {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetWorkPos());
        ImGui::SetNextWindowSize(viewport->GetWorkSize());
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar     | ImGuiWindowFlags_NoDocking
                                     | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoCollapse
                                     | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoResize
                                     | ImGuiWindowFlags_NoNavFocus  | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (ImGui::Begin("DockSpace", nullptr, windowFlags)) {
            ImGui::PopStyleVar(2);
            ImGui::DockSpace(ImGui::GetID("MainDock"), ImVec2(0.0f, 0.0f));

            if (ImGui::BeginMenuBar()) {

                for (auto menu : MenuBarItems)
                    if (ImGui::BeginMenu(menu)) ImGui::EndMenu();

                if (ImGui::BeginMenu("View")) {
                    for (auto &view : ContentRegistry::Views::getEntries()) {
                        if (view->hasViewMenuItemEntry())
                            ImGui::MenuItem((std::string(view->getName()) + " View").c_str(), "", &view->getWindowOpenState());
                    }
                    ImGui::EndMenu();
                }

                for (auto &view : ContentRegistry::Views::getEntries()) {
                    view->drawMenu();
                }

                if (ImGui::BeginMenu("View")) {
                    ImGui::Separator();
                    ImGui::MenuItem("Display FPS", "", &this->m_fpsVisible);
                    #ifdef DEBUG
                        ImGui::MenuItem("Demo View", "", &this->m_demoWindowOpen);
                    #endif
                    ImGui::EndMenu();
                }

                if (this->m_fpsVisible) {
                    char buffer[0x20];
                    snprintf(buffer, 0x20, "%.1f FPS", ImGui::GetIO().Framerate);

                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFontSize() * strlen(buffer) + 20);
                    ImGui::TextUnformatted(buffer);
                }

                ImGui::EndMenuBar();
            }

            if (auto &[key, mods] = Window::s_currShortcut; key != -1) {
                for (auto &view : ContentRegistry::Views::getEntries()) {
                    if (view->getWindowOpenState()) {
                        if (view->handleShortcut(key, mods))
                            break;
                    }
                }

                Window::s_currShortcut = { -1, -1 };
            }

            bool anyViewOpen = false;
            for (auto &view : ContentRegistry::Views::getEntries())
                anyViewOpen = anyViewOpen || (view->getWindowOpenState() && view->isAvailable());

            if (!anyViewOpen) {
                char title[256];
                ImFormatString(title, IM_ARRAYSIZE(title), "%s/DockSpace_%08X", ImGui::GetCurrentWindow()->Name, ImGui::GetID("MainDock"));
                if (ImGui::Begin(title)) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * this->m_globalScale, 10 * this->m_globalScale));
                    if (ImGui::BeginChild("Welcome Screen", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoDecoration)) {
                        this->drawWelcomeScreen();
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                }
                ImGui::End();
            }

        }
        ImGui::End();
    }

    void Window::frameEnd() {
        ImGui::Render();

        int displayWidth, displayHeight;
        glfwGetFramebufferSize(this->m_window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_current_context);

        glfwSwapBuffers(this->m_window);
    }

    void Window::drawWelcomeScreen() {
        ImGui::UnderlinedText("Welcome to ImHex!", ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));

        ImGui::NewLine();

        auto availableSpace = ImGui::GetContentRegionAvail();

        ImGui::Indent();
        if (ImGui::BeginTable("Welcome Left", 1, ImGuiTableFlags_NoBordersInBody, ImVec2(availableSpace.x / 2, availableSpace.y))) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 100);
            ImGui::TableNextColumn();
            ImGui::Text("Start");
            {
                if (ImGui::BulletHyperlink("Open File"))
                    EventManager::post(Events::OpenWindow, "Open File");
                if (ImGui::BulletHyperlink("Open Project"))
                    EventManager::post(Events::OpenWindow, "Open Project");
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 100);
            ImGui::TableNextColumn();
            ImGui::Text("Recent");
            {
                if (!this->m_recentFiles.empty()) {
                    for (auto &path : this->m_recentFiles) {
                        if (ImGui::BulletHyperlink(std::filesystem::path(path).filename().string().c_str())) {
                            EventManager::post(Events::FileDropped, path.c_str());
                            break;
                        }
                    }
                }
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 100);
            ImGui::TableNextColumn();
            ImGui::Text("Help");
            {
                if (ImGui::BulletHyperlink("GitHub Repository")) hex::openWebpage("https://github.com/WerWolv/ImHex");
                if (ImGui::BulletHyperlink("Get help")) hex::openWebpage("https://github.com/WerWolv/ImHex/discussions/categories/get-help");
            }

            ImGui::EndTable();
        }
        ImGui::SameLine();
        if (ImGui::BeginTable("Welcome Right", 1, ImGuiTableFlags_NoBordersInBody, ImVec2(availableSpace.x / 2, availableSpace.y))) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 100);
            ImGui::TableNextColumn();
            ImGui::Text("Customize");
            {
                if (ImGui::DescriptionButton("Settings", "Change preferences of ImHex", ImVec2(ImGui::GetContentRegionAvail().x * 0.8f, 0)))
                    EventManager::post(Events::OpenWindow, "Preferences");
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 100);
            ImGui::TableNextColumn();
            ImGui::Text("Learn");
            {
                if (ImGui::DescriptionButton("Latest Release", "Get the latest version of ImHex or read the current changelog", ImVec2(ImGui::GetContentRegionAvail().x * 0.8, 0)))
                    hex::openWebpage("https://github.com/WerWolv/ImHex/releases/latest");
                if (ImGui::DescriptionButton("Pattern Language Documentation", "Learn how to write ImHex patterns with our extensive documentation", ImVec2(ImGui::GetContentRegionAvail().x * 0.8, 0)))
                    hex::openWebpage("https://github.com/WerWolv/ImHex/wiki/Pattern-Language-Guide");
                if (ImGui::DescriptionButton("Plugins API", "Extend ImHex with additional features using plugins", ImVec2(ImGui::GetContentRegionAvail().x * 0.8, 0)))
                    hex::openWebpage("https://github.com/WerWolv/ImHex/wiki/Plugins-Development-Guide");
            }

            ImGui::EndTable();
        }
    }

     void Window::initGLFW() {
        glfwSetErrorCallback([](int error, const char* desc) {
            fprintf(stderr, "Glfw Error %d: %s\n", error, desc);
        });

        if (!glfwInit())
            throw std::runtime_error("Failed to initialize GLFW!");

        #ifdef __APPLE__
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        #endif

        if (auto *monitor = glfwGetPrimaryMonitor(); monitor) {
            float xscale, yscale;
            glfwGetMonitorContentScale(monitor, &xscale, &yscale);

            // In case the horizontal and vertical scale are different, fall back on the average
            this->m_globalScale = this->m_fontScale = std::midpoint(xscale, yscale);
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);


        this->m_window = glfwCreateWindow(1280 * this->m_globalScale, 720 * this->m_globalScale, "ImHex", nullptr, nullptr);


        if (this->m_window == nullptr)
            throw std::runtime_error("Failed to create window!");

        glfwMakeContextCurrent(this->m_window);
        glfwSwapInterval(1);

         {
             int x = 0, y = 0;
             glfwGetWindowPos(this->m_window, &x, &y);
             SharedData::windowPos = ImVec2(x, y);
         }

         {
             int width = 0, height = 0;
             glfwGetWindowSize(this->m_window, &width, &height);
             SharedData::windowSize = ImVec2(width, height);
         }

         glfwSetWindowPosCallback(this->m_window, [](GLFWwindow *window, int x, int y) {
             SharedData::windowPos = ImVec2(x, y);
         });

        glfwSetWindowSizeCallback(this->m_window, [](GLFWwindow *window, int width, int height) {
            SharedData::windowSize = ImVec2(width, height);
        });

        glfwSetKeyCallback(this->m_window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {
            if (action == GLFW_PRESS) {
                Window::s_currShortcut = { key, mods };
                auto &io = ImGui::GetIO();
                io.KeysDown[key] = true;
                io.KeyCtrl  = (mods & GLFW_MOD_CONTROL) != 0;
                io.KeyShift = (mods & GLFW_MOD_SHIFT) != 0;
                io.KeyAlt   = (mods & GLFW_MOD_ALT) != 0;
            }
            else if (action == GLFW_RELEASE) {
                auto &io = ImGui::GetIO();
                io.KeysDown[key] = false;
                io.KeyCtrl  = (mods & GLFW_MOD_CONTROL) != 0;
                io.KeyShift = (mods & GLFW_MOD_SHIFT) != 0;
                io.KeyAlt   = (mods & GLFW_MOD_ALT) != 0;
            }
        });

        glfwSetDropCallback(this->m_window, [](GLFWwindow *window, int count, const char **paths) {
            if (count != 1)
                return;

            View::postEvent(Events::FileDropped, paths[0]);
        });

        glfwSetWindowCloseCallback(this->m_window, [](GLFWwindow *window) {
            View::postEvent(Events::WindowClosing, window);
        });


        glfwSetWindowSizeLimits(this->m_window, 720, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);

        if (gladLoadGL() == 0)
            throw std::runtime_error("Failed to initialize OpenGL loader!");
    }

    void Window::initImGui() {
        IMGUI_CHECKVERSION();
        auto *ctx = ImGui::CreateContext();
        GImGui = ctx;

        ImGuiIO& io = ImGui::GetIO();
        ImGuiStyle& style = ImGui::GetStyle();

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigViewportsNoTaskBarIcon = true;
        io.KeyMap[ImGuiKey_Tab]         = GLFW_KEY_TAB;
        io.KeyMap[ImGuiKey_LeftArrow]   = GLFW_KEY_LEFT;
        io.KeyMap[ImGuiKey_RightArrow]  = GLFW_KEY_RIGHT;
        io.KeyMap[ImGuiKey_UpArrow]     = GLFW_KEY_UP;
        io.KeyMap[ImGuiKey_DownArrow]   = GLFW_KEY_DOWN;
        io.KeyMap[ImGuiKey_PageUp]      = GLFW_KEY_PAGE_UP;
        io.KeyMap[ImGuiKey_PageDown]    = GLFW_KEY_PAGE_DOWN;
        io.KeyMap[ImGuiKey_Home]        = GLFW_KEY_HOME;
        io.KeyMap[ImGuiKey_End]         = GLFW_KEY_END;
        io.KeyMap[ImGuiKey_Insert]      = GLFW_KEY_INSERT;
        io.KeyMap[ImGuiKey_Delete]      = GLFW_KEY_DELETE;
        io.KeyMap[ImGuiKey_Backspace]   = GLFW_KEY_BACKSPACE;
        io.KeyMap[ImGuiKey_Space]       = GLFW_KEY_SPACE;
        io.KeyMap[ImGuiKey_Enter]       = GLFW_KEY_ENTER;
        io.KeyMap[ImGuiKey_Escape]      = GLFW_KEY_ESCAPE;
        io.KeyMap[ImGuiKey_KeyPadEnter] = GLFW_KEY_KP_ENTER;
        io.KeyMap[ImGuiKey_A]           = GLFW_KEY_A;
        io.KeyMap[ImGuiKey_C]           = GLFW_KEY_C;
        io.KeyMap[ImGuiKey_V]           = GLFW_KEY_V;
        io.KeyMap[ImGuiKey_X]           = GLFW_KEY_X;
        io.KeyMap[ImGuiKey_Y]           = GLFW_KEY_Y;
        io.KeyMap[ImGuiKey_Z]           = GLFW_KEY_Z;

        if (this->m_globalScale != 0.0f)
            style.ScaleAllSizes(this->m_globalScale);

        #if defined(OS_WINDOWS)
            std::filesystem::path resourcePath = std::filesystem::path((SharedData::mainArgv)[0]).parent_path();
        #elif defined(OS_LINUX) || defined(OS_MACOS)
            std::filesystem::path resourcePath = "/usr/share/ImHex";
        #else
            std::filesystem::path resourcePath = "";
            #warning "Unsupported OS for custom font support"
        #endif

        if (!resourcePath.empty() && this->setFont(resourcePath / "font.ttf")) {

        }
        else if ((this->m_fontScale != 0.0f) && (this->m_fontScale != 1.0f)) {
            io.Fonts->Clear();

            ImFontConfig cfg;
            cfg.OversampleH = cfg.OversampleV = 1, cfg.PixelSnapH = true;
            cfg.SizePixels = 13.0f * this->m_fontScale;
            io.Fonts->AddFontDefault(&cfg);
        }

        style.WindowMenuButtonPosition = ImGuiDir_None;
        style.IndentSpacing = 10.0F;

        // Install custom settings handler
        ImGuiSettingsHandler handler;
        handler.TypeName = "ImHex";
        handler.TypeHash = ImHashStr("ImHex");
        handler.ReadOpenFn = ImHexSettingsHandler_ReadOpenFn;
        handler.ReadLineFn = ImHexSettingsHandler_ReadLine;
        handler.WriteAllFn = ImHexSettingsHandler_WriteAll;
        handler.UserData   = this;
        ctx->SettingsHandlers.push_back(handler);

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(this->m_window, true);
        ImGui_ImplOpenGL3_Init("#version 150");
    }

    void Window::initPlugins() {
        try {
            auto pluginFolderPath = std::filesystem::path((SharedData::mainArgv)[0]).parent_path() / "plugins";
            PluginHandler::load(pluginFolderPath.string());
        } catch (std::runtime_error &e) { return; }

        for (const auto &plugin : PluginHandler::getPlugins()) {
            plugin.initializePlugin();
        }
    }

    void Window::deinitGLFW() {
        glfwDestroyWindow(this->m_window);
        glfwTerminate();
    }

    void Window::deinitImGui() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void Window::deinitPlugins() {
        PluginHandler::unload();
    }

}