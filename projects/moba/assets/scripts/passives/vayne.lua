-- 薇恩 被动「圣银弩箭」：每第 3 次普攻造成目标最大生命 6% 的真实伤害。
local M = {}
function M.on_acquire(p) p.owner._vayne = 0 end
function M.on_hit(p, target)
    local o = p.owner
    o._vayne = (o._vayne or 0) + 1
    if o._vayne % 3 == 0 and target ~= nil and not target.dead then
        p.damage(target, target.maxHp * 0.06, "true")
    end
end
return M
