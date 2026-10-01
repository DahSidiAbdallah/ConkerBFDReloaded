-- New game in the empty slot 2, then screenshots of the opening (room 0x18) at its timer.
local shots = { 2128, 2130, 2132, 2134, 2136, 2138, 2140, 2142, 3928, 3930, 3932, 3934, 3936, 3938, 3940, 3942, 5728, 5730, 5732, 5734, 5736, 5738, 5740, 5742 }
local taken = {}
client.speedmode(1600)
while true do
  local r = memory.read_u32_be(0x0BE9F0, "RDRAM")
  local t = memory.read_s32_be(0x0E0A90, "RDRAM")
  if r == 0x1D then
    if t >= 187 and t < 197 then joypad.set({ Start = true }, 1) end
    if t >= 330 and t < 348 then joypad.setanalog({ ["X Axis"] = -127 }, 1) else joypad.setanalog({ ["X Axis"] = 0 }, 1) end
    if (t >= 450 and t < 460) or (t >= 630 and t < 640) then joypad.set({ Start = true }, 1) end
  elseif r == 0x18 then
    for _, s in ipairs(shots) do
      if t >= s and not taken[s] then
        taken[s] = true
        client.screenshot(string.format("C:/ConkerRecompWin/bizhawk/shots_intro/throne%d.png", s))
      end
    end
    if t > 5760 then break end
  end
  if emu.framecount() > 20000 then break end
  emu.frameadvance()
end
client.exit()
