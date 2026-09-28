#include <memory>
#include <string>

#include "cata_catch.h"
#include "options.h"
#include "worldfactory.h"

TEST_CASE( "world_create_timestamp" )
{
    std::unique_ptr<WORLD> world = std::make_unique<WORLD>();
    REQUIRE( world->create_timestamp() );
    INFO( world->timestamp );
    CHECK( world->timestamp.size() + 1 == sizeof( "yyyymmddHHMMSS123456789" ) );
    for( const char ch : world->timestamp ) {
        CHECK( ch >= '0' );
        CHECK( ch <= '9' );
    }
}

TEST_CASE( "world_rescan_keeps_the_active_world", "[worldfactory]" )
{
    WORLD *const active = world_generator->active_world;
    REQUIRE( active != nullptr );
    const std::string name = active->world_name;
    options_manager::cOpt &world_end = get_options().get_option( "WORLD_END" );
    const std::string original_world_end = world_end.getValue();
    const std::string changed_world_end = original_world_end == "keep" ? "reset" : "keep";
    world_end.setValue( changed_world_end );

    // Rescanning the worlds must neither free the active one, which the options manager
    // still reads its options from, nor lose options not saved yet.
    world_generator->init();

    CHECK( world_generator->active_world == active );
    CHECK( world_generator->get_world( name ) == active );
    CHECK( get_options().get_option( "WORLD_END" ).getValue() == changed_world_end );

    get_options().get_option( "WORLD_END" ).setValue( original_world_end );
}
