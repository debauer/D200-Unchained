#pragma once

/* Native tests supply their own configuration, independent of local credentials. */
#ifdef WEB_CONFIG_HEADER
#include WEB_CONFIG_HEADER
#else
#include "web_local.h"
#endif
