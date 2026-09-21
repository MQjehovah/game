-- 拉克丝 被动「光芒四射」：施法后标记附近敌人，下次普攻造成额外魔法伤害。
local M = {}
function M.on_cast(p)
    local list = p.enemiesNear(7)
    if #list > 0 then p.owner._luxMark = list[1] end
end
function M.on_hit(p, target)
    local o = p.owner
    if o._luxMark ~= nil and o._luxMark == target and target ~= nil and not target.dead then
        p.damage(target, 40 + (o.ap or 0) * 0.3, "magic")
        o._luxMark = nil
    end
end
return M
