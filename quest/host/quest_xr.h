// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <jni.h>

namespace gt4::quest
{
// Runs the OpenXR session on the calling thread until the session ends or requestClose().
// `activity` is the NativeActivity's jobject (global ref owned by the caller).
void runXrThread(JavaVM *vm, jobject activity);
} // namespace gt4::quest
