#include "PythonPluginEngine.hpp"
#include "../api/CommandRegistry.hpp"
#include "../api/EventDispatcher.hpp"
#include "../api/Schedule.hpp"
#include "../resource.hpp"
#include "api/utils/FileUtils.hpp"
#include "api/utils/ModuleUtils.hpp"
#include "api/utils/StringUtils.hpp"

// 必须释放全局 GIL 锁
static std::unique_ptr<py::gil_scoped_release> mRelease = nullptr;

PythonPluginEngine::PythonPluginEngine() : IPluginEngine() {
    Py_SetPythonHome(L"./plugins/KobeBryantScriptEngine-Python/Python313");
    mRelease = std::make_unique<py::gil_scoped_release>();
    mI18n    = std::make_unique<i18n::LangI18n>("./plugins/KobeBryantScriptEngine-Python/lang", "zh_CN");
    mI18n->updateOrCreateLanguage("zh_CN", *utils::readCurrentResource(LANG_ZH_CN));
    mI18n->loadAllLanguages();
    mI18n->forEachLangFile([](const std::string& languageCode, const i18n::LangLanguage& language) {
        Logger::appendLanguage(languageCode, language);
    });
};

PythonPluginEngine::~PythonPluginEngine() {
    mI18n.reset();
    mRelease.reset();
    mRelease = nullptr;
}

std::string PythonPluginEngine::getPluginType() const { return "script-python"; }

bool PythonPluginEngine::loadPlugin(std::string const& plugin, std::filesystem::path const& entry) {
    try {
        // 必须先上 GIL 锁
        py::gil_scoped_acquire require{};
        getLogger().info("engine.python.plugin.loading", {plugin});
        // 安装pip包
        std::string requirePath = "./plugins/" + plugin + "/requirements.txt";
        if (std::filesystem::exists(requirePath)) {
            getLogger().info("engine.python.plugin.pip.loading", {plugin});
            system(("pip install -r " + requirePath + " >NUL 2>&1").c_str());
        }
        // 注册模块路径到 sys.path
        py::module sys      = py::module::import("sys");
        py::object sys_path = sys.attr("path");
        sys_path.attr("append")(py::str(entry.parent_path().string()));
        // 构建模块名
        auto moduleName = entry.string();
        utils::ReplaceStr(moduleName, ".py", "");
        utils::ReplaceStr(moduleName, "./", "");
        utils::ReplaceStr(moduleName, "/", ".");
        // 导入模块并启用
        if (!mPluginModules.contains(plugin)) {
            // 如果没有导入过这个模块
            py::module pluginModule = py::module::import(moduleName.c_str());
            // 缓存此模块
            mPluginModules[plugin] = std::move(pluginModule);
            mModuleEntrys[plugin]  = entry;
        } else {
            // 如果导入过这个模块
            py::module_ importlib   = py::module_::import("importlib");
            py::object  reload_func = importlib.attr("reload");
            reload_func(mPluginModules[plugin]);
        }
        // 启用插件
        mPluginModules[plugin].attr("on_enable")();
        // 清理模块路径
        sys_path.attr("remove")(py::str(entry.parent_path().string()));
        getLogger().info("engine.python.plugin.loaded", {plugin});
        return true;
    } catch (const std::exception& e) {
        getLogger().error("engine.python.plugin.load.exception", {plugin, e.what()});
    } catch (...) {
        getLogger().error("engine.python.plugin.load.unknownException", {plugin});
    }
    return false;
}

bool PythonPluginEngine::unloadPlugin(std::string const& plugin) {
    try {
        getLogger().info("engine.python.plugin.unloading", {plugin});
        // 清理资源
        ScriptEventBusImpl::getInstance().removePluginListeners(plugin);
        ScriptSchedule::getInstance().removePluginTasks(plugin);
        ScriptCommandManager::getInstance().removePluginCommands(plugin);
        // 必须先上 GIL 锁
        py::gil_scoped_acquire require{};
        // 禁用插件
        mPluginModules[plugin].attr("on_disable")();
        // 替换为空模块
        auto path = mModuleEntrys[plugin];
        backupEntry(path);
        // 重载模块
        py::module_ importlib   = py::module_::import("importlib");
        py::object  reload_func = importlib.attr("reload");
        reload_func(mPluginModules[plugin]);
        // 还原模块
        resumeEntry(path);
        getLogger().info("engine.python.plugin.unloaded", {plugin});
        return true;
    } catch (const std::exception& e) {
        getLogger().error("engine.python.plugin.unload.exception", {plugin, e.what()});
    } catch (...) {
        getLogger().error("engine.python.plugin.unload.unknownException", {plugin});
    }
    return false;
}

Logger& PythonPluginEngine::getLogger() { return mLogger; }

void PythonPluginEngine::backupEntry(std::filesystem::path const& entry) {
    auto newPath = entry.string();
    newPath.append(".bak");
    if (std::filesystem::exists(newPath)) {
        std::filesystem::remove(newPath);
    }
    if (std::filesystem::exists(entry)) {
        std::filesystem::copy(entry.string(), newPath);
    }
    utils::writeFile(entry, "");
}

void PythonPluginEngine::resumeEntry(std::filesystem::path const& entry) {
    auto newPath = entry.string();
    newPath.append(".bak");
    if (std::filesystem::exists(entry)) {
        std::filesystem::remove(entry);
        std::filesystem::copy_file(newPath, entry);
        std::filesystem::remove(newPath);
    }
}

std::optional<std::string> PythonPluginEngine::getCallingPlugin() {
    py::object stack = py::module::import("inspect").attr("stack");
    py::list   trace = stack();
    // 从堆栈中提取调用者信息
    if (!trace.empty()) {
        py::object         frame_info = trace[0].cast<py::object>();
        py::object         frame      = frame_info.attr("frame");
        py::object         filename   = frame.attr("f_code").attr("co_filename");
        std::string        pluginName = py::str(filename);
        static std::string parentPath = std::filesystem::absolute("./plugins/").string();
        utils::ReplaceStr(pluginName, parentPath, "");
        auto pos = pluginName.find("/");
        if (pos == std::string::npos) {
            pos = pluginName.find("\\");
        }
        if (pos != std::string::npos) {
            pluginName.erase(pos);
        }
        return pluginName;
    }
    return {};
}