-- Load Clean Tone Studio (LV2 and VST3) in a headless Ardour session (made by ardour8-new_empty_session)
AudioEngine:set_backend ("None (Dummy)", "", "")
AudioEngine:start (false)
s = load_session ("/tmp/cts-ardour", "cts-ardour")
assert (s, "session did not load")
local tl = s:new_audio_track (1, 2, nil, 1, "Guitar", ARDOUR.PresentationInfo.max_order, ARDOUR.TrackMode.Normal, true)
local tr = tl:front ()
local function lat (pi)
  for _, f in ipairs ({ function () return pi:effective_latency () end, function () return pi:signal_latency () end,
                        function () return pi:plugin (0):signal_latency () end }) do
    local ok, l = pcall (f); if ok then return l end
  end
  return -1
end
for _, t in ipairs ({ { ARDOUR.PluginType.LV2, "https://aloomens.github.io/clean-tone-studio/", "LV2" },
                      { ARDOUR.PluginType.VST3, "Clean Tone Studio", "VST3" } }) do
  local p = ARDOUR.LuaAPI.new_plugin (s, t[2], t[1], "")
  if p:isnil () then print ("FAIL  Ardour could not create the " .. t[3])
  else
    tr:add_processor_by_index (p, -1, nil, true)
    local pi = p:to_insert ()
    print ("PASS  Ardour loaded the " .. t[3] .. ": " .. pi:name () .. ", latency " .. tostring (lat (pi)) .. " samples, "
           .. tostring (pi:plugin (0):parameter_count ()) .. " ports/parameters")
  end
end
s:save_state ("", false, false, false, false, false)
print ("session saved")
close_session ()
