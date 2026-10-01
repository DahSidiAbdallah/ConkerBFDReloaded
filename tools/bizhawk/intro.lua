-- Screenshots of the Nintendo logo scene (room 0x21) at its own timer.
local shots = { 480, 600, 720, 840, 1320, 1440 }
local taken = {}
client.speedmode(800)
while true do
  local r = memory.read_u32_be(0x0BE9F0, "RDRAM")
  local t = memory.read_s32_be(0x0E0A90, "RDRAM")
  if r == 0x21 then
    for _, s in ipairs(shots) do
      if t >= s and t >= s and t < s + 6 and not taken[s] then
        taken[s] = true
        client.screenshot(string.format("C:/ConkerRecompWin/bizhawk/shots_intro/t%d.png", s))
      end
    end
  elseif r == 0x1D then break end
  if emu.framecount() > 6000 then break end
  emu.frameadvance()
end
client.exit()
