#pragma once

#include <stdio.h>

#include "sc/result.h"

void sc_app_print_version(FILE *stream);
void sc_app_log_bootstrap_failure(const sc_status *status);
void sc_app_print_bootstrap_failure(FILE *stream, const char *command, const sc_status *status);

int sc_app_run_acp();
int sc_app_run_chat();
int sc_app_run_config_preset(const char *preset);
int sc_app_run_config_show();
int sc_app_run_cron_command(int argc, char **argv);
int sc_app_run_daemon(bool hard_shutdown);
int sc_app_run_doctor();
int sc_app_run_estop_command(const char *command);
int sc_app_run_gateway(bool hard_shutdown, const char *bind);
int sc_app_run_init_config();
int sc_app_run_memory();
int sc_app_run_onboard(int argc, char **argv);
int sc_app_run_provider();
int sc_app_run_provider_set_key();
int sc_app_run_service_command(const char *command, const char *argv0);
