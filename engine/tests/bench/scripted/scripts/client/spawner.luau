-- 2,000 clones of `template`: each runs mover.luau (its own onTick and a repeating timer).
local template = entity("template")
local crowd = entity("crowd")
for i = 1, 2000 do
	local c = template:clone(crowd)
	c.position = vec2(20 + (i % 60) * 20, 20 + math.floor(i / 60) * 20)
	c.visible = true
end
