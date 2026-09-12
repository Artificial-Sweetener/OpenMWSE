-- Mirrors test_lua_api under GPU paging.
local testing = require('testing_util')
local util = require('openmw.util')
local types = require('openmw.types')
local world = require('openmw.world')

local function initPlayer(player)
    player = player or world.players[1]
    player:teleport('', util.vector3(4096, 4096, 1745), util.transform.identity)
    coroutine.yield()
    return player
end

testing.setSetupLocalTest(function(player)
    initPlayer(player)
end)

local function runWithSpawnedTarget(name)
    local player = initPlayer()
    local target = world.createObject(types.NPC.record(player).id)
    target:teleport(player.cell, player.position + util.vector3(0, 150, 0), util.transform.rotateZ(math.rad(180)))
    coroutine.yield()
    coroutine.yield()
    -- The spawned NPC dies with the test.
    local ok, err = pcall(testing.runLocalTest, player, name)
    target:remove()
    if not ok then
        error(err, 0)
    end
end

testing.registerGlobalTest('getFocusRay reports crosshair target', function()
    runWithSpawnedTarget('getFocusRay reports crosshair target')
end)

testing.registerGlobalTest('rendering rays should hit animated NPCs', function()
    runWithSpawnedTarget('rendering rays should hit animated NPCs')
end)

return {
    engineHandlers = {
        onUpdate = testing.updateGlobal,
    },
    eventHandlers = testing.globalEventHandlers,
}
