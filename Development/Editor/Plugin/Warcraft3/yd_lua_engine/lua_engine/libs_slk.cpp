#include "storm.h"
#include <base/lua/make_range.h>
#include <slk/ObjectManager.hpp>
#include <slk/InterfaceStorm.hpp>

namespace base { namespace warcraft3 { namespace lua_engine {
	static int slk_object_index(lua::state* ls);
	static int slk_object_pairs(lua::state* ls);
	static int slk_create_proxy_table(lua::state* ls, lua::cfunction index_func, lua::cfunction pairs_func, uintptr_t upvalue);
}}

namespace lua
{
	template <>
	int convert_to_lua(state* ls, const std::string& v)
	{
		ls->pushstring(v.c_str());
		return 1;
	}

	template <>
	int convert_to_lua(state* ls, const slk::SlkValue& v)
	{
		ls->pushstring(v.to_string().c_str());
		return 1;
	}

	template <>
	int convert_to_lua(state* ls, const slk::object_id& v)
	{
		ls->pushstring(v.to_string().c_str());
		return 1;
	}

	template <>
	int convert_to_lua(state* ls, const slk::SlkSingle& v)
	{
		warcraft3::lua_engine::slk_create_proxy_table(
			  ls
			, warcraft3::lua_engine::slk_object_index
			, warcraft3::lua_engine::slk_object_pairs
			, (uintptr_t)&(v)
			);
		return 1;
	}
}

namespace warcraft3 { namespace lua_engine {

	class slk_manager
	{
	public:
		slk_manager(slk::InterfaceStorm& storm)
			: mgr_(storm)
		{ }

		slk::SlkTable& load(slk::ROBJECT_TYPE::ENUM type)
		{
			return mgr_.load_singleton<slk::ROBJECT_TYPE::ENUM, slk::SlkTable>(type);
		}

		std::string const& convert_string(std::string const& str)
		{
			return mgr_.convert_string(str);
		}

		static int destroy(lua::state* ls)
		{
			static_cast<slk_manager*>(ls->touserdata(1))->~slk_manager();
			return 0;
		}

		static int create(lua::state* ls, slk::InterfaceStorm& storm)
		{
			slk_manager* mgr = (slk_manager*)ls->newuserdata(sizeof(slk_manager));
			ls->newtable();
			ls->pushcclosure(slk_manager::destroy, 0);
			ls->setfield(-2, "__gc");
			ls->setmetatable(-2);
			new (mgr)slk_manager(storm);
			ls->setfield(LUA_REGISTRYINDEX, "_JASS_SLK_MGR");
			return 0;
		}

		static slk_manager* get(lua::state* ls)
		{
			ls->getfield(LUA_REGISTRYINDEX, "_JASS_SLK_MGR");
			slk_manager* mgr = (slk_manager*)ls->touserdata(-1);
			ls->pop(1);
			return mgr;
		}

	private:
		slk::ObjectManager  mgr_;
	};

	static int slk_table_newindex(lua::state* /*ls*/)
	{
		return 0;
	}

	static int slk_create_proxy_table(lua::state* ls, lua::cfunction index_func, lua::cfunction pairs_func, uintptr_t upvalue)
	{
		ls->newtable();
		{
			ls->newtable();
			{
				ls->pushstring("__index");
				ls->pushinteger(upvalue);
				ls->pushcclosure(index_func, 1);
				ls->rawset(-3);

				ls->pushstring("__newindex");
				ls->pushcclosure(slk_table_newindex, 0);
				ls->rawset(-3);

				ls->pushstring("__pairs");
				ls->pushinteger(upvalue);
				ls->pushcclosure(pairs_func, 1);
				ls->rawset(-3);
			}
			ls->setmetatable(-2);

			ls->pushstring("factory");
			ls->pushinteger(upvalue);
			ls->pushcclosure(pairs_func, 1);
			ls->rawset(-3);
		}
		return 1;
	}

	static int slk_object_pairs(lua::state* ls)
	{
		slk::SlkSingle* object_ptr = (slk::SlkSingle*)(uintptr_t)ls->tointeger(lua_upvalueindex(1));
		return lua::make_range(ls, *object_ptr);
	}

	static int slk_object_index(lua::state* ls)
	{
		slk::SlkSingle* object_ptr = (slk::SlkSingle*)(uintptr_t)ls->tointeger(lua_upvalueindex(1));
		const char* key = ls->tostring(2);
		auto it = object_ptr->find(key);
		if (it == object_ptr->end())
		{
			ls->pushnil();
			return 1;
		}

		return lua::convert_to_lua(ls, it->second);
	}

	static int slk_table_pairs(lua::state* ls)
	{
		slk::ROBJECT_TYPE::ENUM type = (slk::ROBJECT_TYPE::ENUM)ls->tointeger(lua_upvalueindex(1));
		slk::SlkTable& table = slk_manager::get(ls)->load(type);
		return lua::make_range(ls, table);
	}

	static int slk_table_index(lua::state* ls)
	{
		slk::ROBJECT_TYPE::ENUM type = (slk::ROBJECT_TYPE::ENUM)ls->tointeger(lua_upvalueindex(1));
		slk::object_id id;

		switch (ls->type(2))
		{
		case LUA_TSTRING:	
			id = slk::object_id(std::string_view(ls->tostring(2)));
			break;
		case LUA_TNUMBER:	
			id = slk::object_id((uint32_t)ls->tointeger(2));
			break;
		default:
			ls->pushnil();
			return 1;
		}

		slk::SlkTable& table = slk_manager::get(ls)->load(type);
		auto it = table.find(id);
		if (it == table.end())
		{
			ls->pushnil();
			return 1;
		}

		return lua::convert_to_lua(ls, it->second);
	}

	static int slk_create_table(lua::state* ls, const char* name, slk::ROBJECT_TYPE::ENUM type)
	{
		ls->pushstring(name);
		slk_create_proxy_table(ls, slk_table_index, slk_table_pairs, type);
		ls->rawset(-3);
		return 0;
	}

	class slk_interface_storm
		: public slk::InterfaceStorm
	{
	public:
		slk_interface_storm()
			: s_()
		{ }

		bool has(std::string const& path)
		{
			return s_.has_file(path.c_str());
		}

		std::string load(std::string const& path, error_code& ec)
		{
			const void* buf_data = nullptr;
			size_t      buf_size = 0;

			if (!s_.load_file(path.c_str(), &buf_data, &buf_size))
			{
				ec = ERROR_FILE_NOT_FOUND;
				return std::move(std::string());
			}
			std::string result((const char*)buf_data, ((const char*)buf_data) + buf_size);
			s_.unload_file(buf_data);
			return std::move(result);
		}

		static slk_interface_storm& instance()
		{
			static slk_interface_storm storm;
			return storm;
		}

	private:
		storm s_;
	};

	int jass_slk(lua::state* ls)
	{
		slk_manager::create(ls, slk_interface_storm::instance());

		ls->newtable();
		{
			slk_create_table(ls, "ability", slk::ROBJECT_TYPE::ABILITY);
			slk_create_table(ls, "buff", slk::ROBJECT_TYPE::BUFF);
			slk_create_table(ls, "unit", slk::ROBJECT_TYPE::UNIT);
			slk_create_table(ls, "item", slk::ROBJECT_TYPE::ITEM);
			slk_create_table(ls, "upgrade", slk::ROBJECT_TYPE::UPGRADE);
			slk_create_table(ls, "doodad", slk::ROBJECT_TYPE::DOODAD);
			slk_create_table(ls, "destructable", slk::ROBJECT_TYPE::DESTRUCTABLE);
			slk_create_table(ls, "misc", slk::ROBJECT_TYPE::MISC);
		}
		return 1;
	}
}}}
