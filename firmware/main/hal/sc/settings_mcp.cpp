/*
 * SPDX-FileCopyrightText: 2026 chunhuan-lu/StackChan contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "settings_mcp.h"
#include <stackchan/sc/sc_settings.h>
#include <mcp_server.h>
#include <mooncake_log.h>
#include <cJSON.h>
#include <string>

namespace sc {

using stackchan::sc::ScSettings;
using stackchan::sc::SettingInfo;
using stackchan::sc::SettingType;

static const std::string_view _tag = "SettingsMcp";

static cJSON* describe(const SettingInfo& info, int value)
{
    char text[32];
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "key", info.key);
    cJSON_AddStringToObject(item, "label", info.label);
    cJSON_AddNumberToObject(item, "value", value);
    cJSON_AddStringToObject(item, "shown", ScSettings::format(info, value, text, sizeof(text)));
    cJSON_AddNumberToObject(item, "min", info.min);
    cJSON_AddNumberToObject(item, "max", info.max);
    cJSON_AddNumberToObject(item, "default", info.def);
    if (info.type == SettingType::Bool) {
        cJSON_AddStringToObject(item, "type", "on_off");
    } else if (info.type == SettingType::Choice) {
        cJSON* choices = cJSON_CreateArray();
        for (int v = info.min; v <= info.max; ++v) {
            cJSON_AddItemToArray(choices, cJSON_CreateString(info.choices[v - info.min]));
        }
        cJSON_AddItemToObject(item, "choices", choices);
    }
    cJSON_AddStringToObject(item, "help", info.help);
    return item;
}

void add_settings_mcp_tools()
{
    auto& mcp_server = McpServer::GetInstance();

    mclog::tagInfo(_tag, "add robot.get_settings tool");
    mcp_server.AddTool("self.robot.get_settings",
                       "List the robot's own adjustable settings: idle activity, sleep schedule, sleep screen and light "
                       "brightness, drawn faces, tap to wake, the Claude Code display, standby on USB power. Each has "
                       "a key, the current value, min/max and the default; on_off settings use 1 = on, 0 = off.",
                       std::vector<Property>{}, [](const PropertyList&) -> ReturnValue {
                           auto& settings = ScSettings::get();
                           cJSON* list    = cJSON_CreateArray();
                           for (const auto* info : settings.available()) {
                               cJSON_AddItemToArray(list, describe(*info, settings.value(info->id)));
                           }
                           return list;
                       });

    mclog::tagInfo(_tag, "add robot.set_setting tool");
    mcp_server.AddTool("self.robot.set_setting",
                       "Change one of the robot's settings when the user asks for it. Call get_settings first for the "
                       "keys and ranges. The value is clamped to the range; the result is the value now in use. "
                       "Changes apply at once and survive a restart.",
                       PropertyList({Property("key", kPropertyTypeString), Property("value", kPropertyTypeInteger)}),
                       [](const PropertyList& properties) -> ReturnValue {
                           const auto key  = properties["key"].value<std::string>();
                           const int value = properties["value"].value<int>();
                           auto& settings  = ScSettings::get();
                           const SettingInfo* info = settings.find(key.c_str());
                           if (!info) {
                               mclog::tagWarn(_tag, "set_setting: unknown key {}", key);
                               return std::string("Unknown setting key: ") + key + ". Call get_settings for the keys.";
                           }
                           settings.set(info->id, value);
                           mclog::tagInfo(_tag, "set_setting: {} = {} (asked {})", info->key,
                                          settings.value(info->id), value);
                           return describe(*info, settings.value(info->id));
                       });
}

}  // namespace sc
