-- oop: metatable classes, inheritance, method calls, __add, closures
local NAME = "oop"
local STEPS = OOP_STEPS or 2000
local Vec = {}; Vec.__index = Vec
function Vec.new(x, y) return setmetatable({ x = x, y = y }, Vec) end
function Vec:add(o) return Vec.new(self.x + o.x, self.y + o.y) end
function Vec:scale(s) return Vec.new(self.x * s, self.y * s) end
function Vec:dot(o) return self.x * o.x + self.y * o.y end
Vec.__add = function(a, b) return Vec.new(a.x + b.x, a.y + b.y) end

local Entity = {}; Entity.__index = Entity
function Entity.new(x, y)
  local e = setmetatable({}, Entity)
  e.pos = Vec.new(x, y); e.vel = Vec.new(1, 0.5); e.hp = 100
  return e
end
function Entity:update(dt)
  self.pos = self.pos:add(self.vel:scale(dt))
  if self.pos.x > 100 or self.pos.x < 0 then self.vel = Vec.new(-self.vel.x, self.vel.y) end
  if self.pos.y > 50 or self.pos.y < 0 then self.vel = Vec.new(self.vel.x, -self.vel.y) end
end
function Entity:damage(n) self.hp = self.hp - n; if self.hp < 0 then self.hp = 100 end return self.hp end
function Entity:energy() return self.vel:dot(self.vel) end

local Player = setmetatable({}, { __index = Entity }); Player.__index = Player
function Player.new(x, y, name)
  local p = Entity.new(x, y); setmetatable(p, Player)
  p.name = name; p.score = 0
  return p
end
function Player:update(dt) Entity.update(self, dt); self.score = self.score + 1 end

local function makeCounter() local c = 0; return function(n) c = c + n; return c end end

local function run()
  local ents = {}
  for i = 1, 100 do
    if i % 4 == 0 then ents[i] = Player.new(i, i % 50, "p" .. i) else ents[i] = Entity.new(i % 100, i % 50) end
  end
  local counters = {}
  for i = 1, 10 do counters[i] = makeCounter() end
  local acc = Vec.new(0, 0)
  local e_sum = 0
  for step = 1, STEPS do
    for i = 1, #ents do
      local e = ents[i]
      e:update(0.5)
      e_sum = e_sum + e:energy() + e:damage(i % 3)
      counters[i % 10 + 1](1)
    end
    acc = acc + ents[step % 100 + 1].pos
  end
  local csum = 0
  for i = 1, 10 do csum = csum + counters[i](0) end
  return string.format("%.3f %.3f %.0f %.0f", acc.x, acc.y, e_sum, csum)
end
