-- Mirrors test_lua_api under GPU paging.
local testing = require('testing_util')
local async = require('openmw.async')
local self = require('openmw.self')
local util = require('openmw.util')
local core = require('openmw.core')
local types = require('openmw.types')
local nearby = require('openmw.nearby')
local camera = require('openmw.camera')

types.Player.setControlSwitch(self, types.Player.CONTROL_SWITCH.Controls, false)
types.Player.setControlSwitch(self, types.Player.CONTROL_SWITCH.Fighting, false)
types.Player.setControlSwitch(self, types.Player.CONTROL_SWITCH.Jumping, false)

testing.registerLocalTestStep('getFocusRay reports crosshair target',
    function()
        local target = nil
        for _, actor in ipairs(nearby.actors) do
            if actor.id ~= self.id and (actor.position - self.position):length() < 200 then
                target = actor
            end
        end
        testing.expect(target, 'An NPC should stand ahead of the player')

        local ray = camera.getFocusRay()
        testing.expect(ray.hit, 'Focus ray should hit the NPC ahead')
        testing.expect(ray.hitObject, 'Focus ray should report the NPC it hits')
        testing.expectEqual(ray.hitObject.id, target.id, 'Focus ray should hit the NPC ahead')
        testing.expectLessOrEqual((ray.hitPos - target.position):length(), 200,
            'Focus ray hit position ' .. testing.formatActualExpected(ray.hitPos, target.position))
        testing.expectGreaterThan((ray.hitPos - self.position):length(), 50,
            'Focus ray hit position ' .. testing.formatActualExpected(ray.hitPos, self.position))
    end)

testing.registerLocalTestStep('rendering rays should hit animated NPCs',
    function()
        local target = nil
        local expected = self.position + util.vector3(0, 150, 0)
        local nearest = math.huge
        for _, actor in ipairs(nearby.actors) do
            local distance = (actor.position - expected):length()
            if actor.id ~= self.id and distance < nearest then
                target = actor
                nearest = distance
            end
        end
        testing.expect(target, 'An NPC should stand ahead')
        testing.expectLessOrEqual(nearest, 5, 'Spawned NPC should stand ahead')

        local center = target.position + util.vector3(0, 0, 80)
        local from = center + util.vector3(-100, 0, 0)
        local to = center + util.vector3(100, 0, 0)
        local hit = nearby.castRenderingRay(from, to, { ignore = self })
        testing.expect(hit.hit, 'Sync ray should hit NPC')
        testing.expectEqual(hit.hitObject, target, 'Sync ray should report NPC')
        testing.expectLessOrEqual((hit.hitPos - center):length(), 100, 'Sync hit should lie on ray')
        testing.expectEqualWithDelta(hit.hitPos.y, center.y, 0.01, 'Sync hit y should match ray')
        testing.expectEqualWithDelta(hit.hitPos.z, center.z, 0.01, 'Sync hit z should match ray')
        testing.expectEqualWithDelta(hit.hitNormal:length(), 1, 0.01, 'Sync normal should be unit length')

        local ignored = nearby.castRenderingRay(from, to, { ignore = target })
        testing.expect(ignored.hitObject ~= target, 'Sync ignore should skip NPC')

        local asyncHit = nil
        local asyncIgnored = nil
        nearby.asyncCastRenderingRay(async:callback(function(result) asyncHit = result end), from, to, { ignore = self })
        nearby.asyncCastRenderingRay(
            async:callback(function(result) asyncIgnored = result end), from, to, { ignore = target })
        testing.expect(asyncHit == nil and asyncIgnored == nil, 'Async callbacks should be queued')
        local deadline = core.getRealTime() + 3
        while (asyncHit == nil or asyncIgnored == nil) and core.getRealTime() < deadline do
            coroutine.yield()
        end
        testing.expect(asyncHit, 'Async ray should finish')
        testing.expect(asyncIgnored, 'Ignored async ray should finish')
        testing.expect(asyncHit.hit, 'Async ray should hit NPC')
        testing.expectEqual(asyncHit.hitObject, target, 'Async ray should report NPC')
        testing.expectLessOrEqual((asyncHit.hitPos - center):length(), 100, 'Async hit should lie on ray')
        testing.expectEqualWithDelta(asyncHit.hitPos.y, center.y, 0.01, 'Async hit y should match ray')
        testing.expectEqualWithDelta(asyncHit.hitPos.z, center.z, 0.01, 'Async hit z should match ray')
        testing.expectEqualWithDelta(asyncHit.hitNormal:length(), 1, 0.01, 'Async normal should be unit length')
        testing.expectLessOrEqual((asyncHit.hitPos - hit.hitPos):length(), 1, 'Async and sync hits should agree')
        testing.expect(asyncIgnored.hitObject ~= target, 'Async ignore should skip NPC')
    end)

return {
    engineHandlers = {
        onFrame = testing.updateLocal,
    },
    eventHandlers = testing.localEventHandlers,
}
