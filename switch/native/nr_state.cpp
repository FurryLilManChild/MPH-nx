// MPH-nx native19: save states on the native engine.
//
// The engine's own save states (savestate.cpp) capture memory, both CPUs,
// devices, video and sound. The native engine adds two rules:
//  - save only while both CPUs are parked at a service point, where all
//    guest registers are in the CPU state (not in generated code's locals);
//  - after a load, restart both native coroutines at the loaded PCs.
#include "nr_state.h"
#include "savestate.h"
#include "savestate_slots.h"
#include <cstdio>

extern "C" bool nr_state_parked(void);
extern "C" bool nr7_state_parked(void);
extern "C" void nr_state_restart(void);
extern "C" void nr7_state_restart(void);

bool nx_state_ready() { return nr_state_parked() && nr7_state_parked(); }

bool nx_state_save(const std::string& path, const NdsSavestateIdentity& id, std::string* error) {
    if (!nx_state_ready()) {
        if (error) *error = "the game is mid-instruction; try again";
        return false;
    }
    return nds_savestate_save_core(path, id, error);
}

bool nx_state_load(const std::string& path, const NdsSavestateIdentity& id, std::string* error) {
    if (!nds_savestate_load_core(path, id, error)) return false;
    nds_savestate_reset_host_history();
    nr_state_restart();
    nr7_state_restart();
    return true;
}

void nx_state_after_load() {
    nr_state_restart();
    nr7_state_restart();
}
