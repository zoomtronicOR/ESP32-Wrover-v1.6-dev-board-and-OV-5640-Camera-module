#pragma once

#include <stdbool.h>
#include "cJSON.h"

// Configuration backup (spec §27). Export collects every module's settings into one JSON;
// passwords, tokens and keys only when secrets is set. Login settings are never exported or
// imported, so a restored backup cannot lock the user out.
cJSON *backup_export(bool secrets);
// Applies and saves every section present; returns {"applied":[...],"errors":{section:msg}}.
cJSON *backup_import(const cJSON *cfg);
