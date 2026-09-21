-- 易大师 被动「双重打击」：每第 3 次普攻额外造成一次等量伤害。
local M = {}
function M.on_acquire(p) p.owner._yi = 0 end
function M.on_hit(p, target, amount)
    local o = p.owner
    o._yi = (o._yi or 0) + 1
    if o._yi % 3 == 0 and target ~= nil and not target.dead then
        p.damage(target, amount, "physical")
        p.float("双重打击", true, 0.7, 1.0, 0.8)
    end
end
return M
