// MPH-nx native19: save states on the native engine (nr_state.cpp).
#pragma once
#include <string>
struct NdsSavestateIdentity;
bool nx_state_ready();
bool nx_state_save(const std::string& path, const NdsSavestateIdentity& id, std::string* error);
bool nx_state_load(const std::string& path, const NdsSavestateIdentity& id, std::string* error);
// After the engine loaded a state (savestate slots): restart both native CPUs.
void nx_state_after_load();
