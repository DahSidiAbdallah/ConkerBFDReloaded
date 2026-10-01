-- Screenshot of the bar menu (GAME1) in the street/pub room 0x1D: Start at its timer 187
-- (as our runs), then pictures at 330 and 400.
local taken = {}
client.speedmode(800)
while true do
  local r = memory.read_u32_be(0x0BE9F0, "RDRAM")
  local t = memory.read_s32_be(0x0E0A90, "RDRAM")
  if r == 0x1D then
    if t >= 187 and t < 197 then joypad.set({ Start = true }, 1) end
    for _, s in ipairs({ 330, 400 }) do
      if t >= s and not taken[s] then
        taken[s] = true
        client.screenshot(string.format("C:/ConkerRecompWin/bizhawk/shots_intro/pub%d.png", s))
      end
    end
    if t > 410 then break end
  end
  if emu.framecount() > 8000 then break end
  emu.frameadvance()
end
client.exit()
