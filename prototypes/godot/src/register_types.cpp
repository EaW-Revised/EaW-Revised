#include "prototype_host.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {

void initialize_eawr_godot(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        GDREGISTER_CLASS(eawr::godot_prototype::EawrGodotPrototype);
    }
}

void uninitialize_eawr_godot(const ModuleInitializationLevel) {}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_godot_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_eawr_godot);
    init.register_terminator(uninitialize_eawr_godot);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}

