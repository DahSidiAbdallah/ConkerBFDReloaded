-- Plays the same route as our test runs and takes screenshots at the same moments,
-- timed by the game's own state: Start is pressed every 1.5 s until the first level
-- (room 0x0C) loads; [now: Start at the street scene's timer 187/427/667, as our runs do] then, by the level's timer (screen refreshes since it began):
-- screenshot at 391, A (jump) at 511 with screenshots 3/6/9/12/18 refreshes later,
-- screenshot at 631, Start (pause) at 751 and a screenshot of the pause at 871.
local function room() return memory.read_u32_be(0x0BE9F0, "RDRAM") end
local function timer() return memory.read_s32_be(0x0E0A90, "RDRAM") end
local shots = { 391, 514, 517, 520, 523, 529, 631, 871 }
local log = io.open("C:/ConkerRecompWin/bizhawk/shots_cmp/log.txt", "w")
client.speedmode(800)
local taken = {}
local armed = false -- the level's timer restarts shortly after the room changes
while true do
  local f, r, t = emu.framecount(), room(), timer()
  if r == 0x1D then
    -- As in our runs: Start 4, 8 and 12 s after... (street scene timer 187, 427, 667)
    for _, s in ipairs({ 187, 427, 667 }) do
      if t >= s and t < s + 10 then joypad.set({ Start = true }, 1) end
    end
  elseif r == 0x0C and (armed or t < 300) then
    armed = true
    if t >= 511 and t < 520 then joypad.set({ A = true }, 1) end
    if t >= 751 and t < 761 then joypad.set({ Start = true }, 1) end
    for _, s in ipairs(shots) do
      if t >= s and not taken[s] then
        taken[s] = true
        client.screenshot(string.format("C:/ConkerRecompWin/bizhawk/shots_cmp/t%d.png", s))
        log:write(string.format("shot %d at timer %d\n", s, t))
      end
    end
    if t > 880 then break end
  end
  if f % 300 == 0 then log:write(string.format("frame %d room %X timer %d\n", f, r, t)); log:flush() end
  if f > 20000 then break end
  emu.frameadvance()
end
log:close()
client.exit()
