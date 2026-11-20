-- 50 rotating groups of 100 animated sprites, 200 dynamic bodies in a box, 100 scripted walkers.
local GROUPS, PER_GROUP, BODIES, WALKERS = 50, 100, 200, 100

local crowd = entity("crowd")
local groups = {}
for g = 1, GROUPS do
	local group = spawn("Node2D", { position = vec2(64 + ((g - 1) % 10) * 128, 72 + math.floor((g - 1) / 10) * 144) }, crowd)
	for i = 1, PER_GROUP do
		local a = i * 0.7
		local r = 8 + (i % 25) * 2.2
		spawn("Sprite2D", {
			position = vec2(math.cos(a) * r, math.sin(a) * r),
			size = vec2(12, 12),
			anims = "data/anims/slime.json",
			anim = "wobble",
		}, group)
	end
	groups[g] = group
end

local bodies = entity("bodies")
for i = 1, BODIES do
	local b = spawn("Body2D", {
		position = vec2(40 + (i % 40) * 30, 60 + math.floor(i / 40) * 40),
		body = "dynamic",
		shape = if i % 2 == 0 then "circle" else "rect",
		size = vec2(14, 14),
		bounce = 0.6,
	}, bodies)
	b.velocity = vec2(random(-200, 200), random(-200, 200))
	spawn("Sprite2D", { size = vec2(14, 14), texture = "asset:coin" }, b)
end

for i = 1, WALKERS do
	local w = spawn("Sprite2D", { position = vec2(random(40, 1240), random(40, 680)), size = vec2(10, 10), color = color("#ffcd75") }, crowd)
	local vx, vy = random(-80, 80), random(-80, 80)
	onTick(function(dt)
		local p = w.position
		local x, y = p.x + vx * dt, p.y + vy * dt
		if x < 20 or x > 1260 then vx = -vx end
		if y < 20 or y > 700 then vy = -vy end
		w.position = vec2(x, y)
	end)
end

local hud = entity("hud")
local t = 0
onTick(function(dt)
	t += dt
	for g, group in groups do
		group.rotation = (t * 20 + g * 7) % 360
	end
	hud.text = `t={math.floor(t)}`
end)
