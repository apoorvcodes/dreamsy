local e = script.entity
local dir = 1
every(0.5 + random() * 0.5, function() dir = -dir end)
onTick(function(dt)
	if not e.visible then return end
	local p = e.position
	e.position = vec2(p.x + dir * 30 * dt, p.y)
end)
