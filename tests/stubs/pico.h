#pragma once

// Host-only SDK surface for testing the actual graphics text implementation.
// This directory must never be added to a firmware target's include path.
#define __time_critical_func(name) name
