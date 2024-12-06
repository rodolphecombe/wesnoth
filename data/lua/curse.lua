local _ = wesnoth.textdomain "wesnoth-help"
local on_event = wesnoth.require("on_event")
local old_unit_status = wesnoth.interface.game_display.unit_status

function wesnoth.interface.game_display.unit_status()
    local u = wesnoth.interface.get_displayed_unit()
    if not u then return {} end
    local s = old_unit_status()

    if u.status.cursed then
		table.insert(s, wml.tag.element{ 
            image = "misc/cursed.png",
			tooltip = _ "cursed: This unit is cursed. Its physical and elemental resistances are reduced by 10%."
		})
	end

    return s
end

local function on_hit(weapon, opponent)
    local text
    if opponent.gender == "female" then
        text = _ "female^cursed"
    else
        text = _ "cursed"
    end
    local color = stringx.join(',', {'121', '0', '178'})
    if not wesnoth.interface.is_skipping_messages() then
        wesnoth.interface.float_label(opponent.x, opponent.y, text, color)
    end
    opponent:add_modification('object', {
        -- id = 'curse_object',
        duration = 'turn end',
        wml.tag.effect{
            apply_to = 'image_mod',
            replace = 'CS(25,0,50)'
        },
        wml.tag.effect{
            apply_to = 'resistance',
            wml.tag.resistance{
            	blade=10,
		        pierce=10,
		        impact=10,
		        fire=10,
		        cold=10,
            }
        },
        wml.tag.effect{
            apply_to = 'status',
            add = 'cursed'
        }
    })
    -- opponent.variables.curse_side = wesnoth.current.side
end

local weapon_filter = {special_type_active = 'curse'}
local unit_filter = {wml.tag["not"]{status = 'cursed'}}

wesnoth.game_events.add{
    name = 'attacker_hits',
    first_time_only = false,
    filter = {
        attack = weapon_filter,
        second_unit = unit_filter
    },
    action = function()
        local ctx = wesnoth.current.event_context
        local weapon = wesnoth.units.create_weapon(wml.get_child(ctx, 'weapon'))
        local opponent = wesnoth.units.get(ctx.x2, ctx.y2)
        on_hit(weapon, opponent)
    end
}

wesnoth.game_events.add{
    name = 'defender_hits',
    first_time_only = false,
    filter = {
        second_attack = weapon_filter,
        unit = unit_filter
    },
    action = function()
        local ctx = wesnoth.current.event_context
        local weapon = wesnoth.units.create_weapon(wml.get_child(ctx, 'second_weapon'))
        local opponent = wesnoth.units.get(ctx.x1, ctx.y1)
        on_hit(weapon, opponent)
    end
}

-- Dubious hack to remove the effect at the right time
--[[wesnoth.game_events.add_repeating("side turn", function(ctx)
    for index, cursed_unit in ipairs(wesnoth.units.find_on_map{wml.tag.filter_wml{wml.tag.variables{curse_side = wesnoth.current.side}}}) do
            wesnoth.wml_actions.remove_object{id = cursed_unit.id, object_id = 'curse_object'}
            cursed_unit.status.cursed = false
            wesnoth.wml_actions.clear_variable({name = 'curse_side'}, cursed_unit.variables)
        -- end
    end
end)--]]
wesnoth.game_events.add_repeating("side turn end", function(ctx)
    local all_my_units = wesnoth.units.find{side = wesnoth.current.side}
    for i = 1, #all_my_units do
        all_my_units[i].status.cursed = false
    end
end)