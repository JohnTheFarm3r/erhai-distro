-- SPDX-License-Identifier: MIT
-- Default audio output on the tablet:
--  * headphones and other outputs that are plugged in or connected
--    (Bluetooth, USB) become the default when they appear; when they go
--    away the usual selection falls back to the speakers;
--  * the raw speaker sink is never the default: the protected "Speakers"
--    sink is chosen in its place whenever it exists;
--  * a microphone is the default input whenever there is one.

log = Log.open_topic ("s-erhai-default-output")

local RAW_SPEAKER = "^alsa_output%..*HiFi__Speaker__sink$"
local PROTECTED_SPEAKER = "erhai.speaker"

local function connected_output (name)
  return name:match ("^bluez_output%.") or name:match ("^alsa_output%.usb%-")
end

SimpleEventHook {
  name = "erhai/switch-to-connected-output",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "session-item-added" },
      Constraint { "event.session-item.interface", "=", "linkable" },
      Constraint { "media.class", "=", "Audio/Sink" },
    },
  },
  execute = function (event)
    local name = event:get_subject ().properties ["node.name"]
    if not name or not connected_output (name) then
      return
    end
    local om = event:get_source ():call ("get-object-manager", "metadata")
    local metadata = om:lookup { Constraint { "metadata.name", "=", "default" } }
    if metadata == nil then
      return
    end
    log:info ("new output " .. name .. ", making it the default")
    metadata:set (0, "default.configured.audio.sink", "Spa:String:JSON",
        Json.Object { ["name"] = name }:to_string ())
  end
}:register ()

SimpleEventHook {
  name = "erhai/protect-speakers",
  after = { "default-nodes/find-best-default-node",
            "default-nodes/find-selected-default-node",
            "default-nodes/find-stored-default-node" },
  before = "default-nodes/apply-default-node",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-default-node" },
      Constraint { "default-node.type", "=", "audio.sink" },
    },
  },
  execute = function (event)
    local selected = event:get_data ("selected-node")
    if not selected or not selected:match (RAW_SPEAKER) then
      return
    end
    local available = event:get_data ("available-nodes")
    available = available and available:parse () or {}
    for _, props in ipairs (available) do
      if props ["node.name"] == PROTECTED_SPEAKER then
        log:info ("raw speakers selected, using the protected sink")
        event:set_data ("selected-node", PROTECTED_SPEAKER)
        return
      end
    end
  end
}:register ()

-- The default input is a microphone whenever there is one. Output sinks are
-- also input candidates (for recording what they play), and the protected
-- "Speakers" sink has the higher priority, so without this the speakers'
-- monitor would become the default input.
SimpleEventHook {
  name = "erhai/prefer-microphones",
  after = { "default-nodes/find-best-default-node",
            "default-nodes/find-selected-default-node",
            "default-nodes/find-stored-default-node" },
  before = "default-nodes/apply-default-node",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "select-default-node" },
      Constraint { "default-node.type", "=", "audio.source" },
    },
  },
  execute = function (event)
    local selected = event:get_data ("selected-node")
    local available = event:get_data ("available-nodes")
    available = available and available:parse () or {}
    local best, best_prio
    for _, props in ipairs (available) do
      local class = props ["media.class"] or ""
      if props ["node.name"] == selected and class:match ("^Audio/Source") then
        return
      end
      if class:match ("^Audio/Source") then
        local prio = tonumber (props ["priority.session"]) or 0
        if not best_prio or prio > best_prio then
          best, best_prio = props ["node.name"], prio
        end
      end
    end
    if best then
      log:info ("input " .. tostring (selected) .. " is not a microphone, using " .. best)
      event:set_data ("selected-node", best)
    end
  end
}:register ()
