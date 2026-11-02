/* SPDX-License-Identifier: MIT
 * Independent 22H374 codec adapter: audio processes do not load UI hooks,
 * and existing UI clients acquire no new AudioToolbox dependency.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include "audio_codecs18.h"
#include "audio_clock18.h"
