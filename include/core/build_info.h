#pragma once

#include <stdio.h>

const char *sc_build_version(void);
unsigned int sc_build_abi_version(void);
int sc_build_feature_enabled(const char *name);
void sc_build_features_write(FILE *stream);
void sc_build_capabilities_write(FILE *stream);
