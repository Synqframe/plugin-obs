// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Synqframe

#pragma once

struct obs_service;

namespace synqframe {
inline constexpr const char *ServiceId = "synqframe";
void registerService();
bool isService(const obs_service *service);
}
