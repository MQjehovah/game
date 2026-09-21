-- 盖伦 被动「坚韧」：脱战 6 秒后每秒回复 2% 最大生命值。
local M = {}
function M.on_tick(p, dt)
    local o = p.owner
    if o.dead then return end
    if o.hp < o.maxHp and p.outOfCombat(6) then
        p.heal(o.maxHp * 0.02 * dt)
    end
end
return M
