#include <iostream>
#include <imgui/imgui.h>
#include <imgui-sfml/imgui-SFML.h>
#include <imgui/misc/freetype/imgui_freetype.h>
#include <SFML/Graphics.hpp>
#include <nauth/auth.hpp>
#include <nauth/steam_auth.hpp>
#include <nauth/steam_api.hpp>
#include <networking/networking.hpp>
#include <nentity/camera.hpp>
#include <nentity/entity.hpp>

bool skip_keyboard_input(bool has_focus)
{
    if(!has_focus)
        return true;

    if(ImGui::GetCurrentContext() == nullptr)
        return false;

    bool skip = ImGui::GetIO().WantCaptureKeyboard;

    return skip;
}

template<int c>
bool once(sf::Keyboard::Key k, bool has_focus)
{
    static std::map<sf::Keyboard::Key, bool> last;

    if(!has_focus)
    {
        for(auto& i : last)
            i.second = false;

        return false;
    }

    bool skip = skip_keyboard_input(has_focus);

    sf::Keyboard key;

    if(key.isKeyPressed(k) && !skip && !last[k])
    {
        last[k] = true;

        return true;
    }

    if(!key.isKeyPressed(k))
    {
        last[k] = false;
    }

    return false;
}

template<int c>
bool once(sf::Mouse::Button b, bool has_focus)
{
    static std::map<sf::Mouse::Button, bool> last;

    if(!has_focus)
    {
        for(auto& i : last)
            i.second = false;

        return false;
    }

    sf::Mouse mouse;

    if(mouse.isButtonPressed(b) && !last[b])
    {
        last[b] = true;

        return true;
    }

    if(!mouse.isButtonPressed(b))
    {
        last[b] = false;
    }

    return false;
}

#define ONCE_MACRO(x, y) once<__COUNTER__>(x, y)

void db_pid_saver(size_t cur, size_t requested, void* udata)
{
    assert(udata);

    db_backend& db = *(db_backend*)udata;

    db_read_write tx(db, DB_PERSIST_ID);

    std::string data;
    data.resize(sizeof(size_t));

    size_t to_write = cur + requested;

    memcpy(&data[0], &to_write, sizeof(size_t));

    tx.write("pid", data);
}

struct auth_data : serialisable
{
    bool default_init = false;

    SERIALISE_SIGNATURE(auth_data)
    {
        DO_SERIALISE(default_init);
    }
};

struct client_input : serialisable
{
    SERIALISE_SIGNATURE(client_input)
    {

    }
};

struct server_data : serialisable
{
    SERIALISE_SIGNATURE(server_data)
    {

    }
};

void server_thread()
{
    set_db_location("./db");
    set_num_dbs(3);

    connection conn;
    conn.host("192.168.0.54", 11000);
    set_pid_callback(db_pid_saver);
    set_pid_udata((void*)&get_db());

    {
        size_t persist_id_saved = 0;

        db_read_write tx(get_db(), DB_PERSIST_ID);

        std::optional<db_data> opt = tx.read("pid");

        if(opt)
        {
            db_data& dat = opt.value();

            if(dat.data.size() > 0)
            {
                assert(dat.data.size() == sizeof(size_t));

                memcpy(&persist_id_saved, &dat.data[0], sizeof(size_t));

                set_next_persistent_id(persist_id_saved);

                std::cout << "loaded pid " << persist_id_saved << std::endl;
            }
        }

        if(persist_id_saved < 1024)
            persist_id_saved = 1024;
    }

    auth_manager<auth_data> auth_manage;

    std::string secret_key = "secret/akey.ect";
    uint64_t net_code_appid = 814820;

    set_app_description({secret_key, net_code_appid});

    network_data_model<server_data> data_model;

    int stagger_id = 0;

    sf::Clock server_elapsed;

    while(1)
    {
        double dt_s = server_elapsed.getElapsedTime().asMicroseconds() / 1000. / 1000.;

        dt_s = clamp(dt_s, 1/1000., 32/1000.);

        while(conn.has_new_client())
        {
            conn.pop_new_client();
        }

        while(conn.has_read())
        {
            nlohmann::json network_json;
            uint64_t read_id = -1;

            std::optional<auth<auth_data>*> found_auth;

            network_protocol proto;

            {
                read_id = conn.reads_from(proto);

                if(proto.type == network_mode::STEAM_AUTH)
                {
                    std::string hex = proto.data;

                    auth_manage.handle_steam_auth(read_id, hex, get_db());
                }

                if(!auth_manage.authenticated(read_id))
                {
                    printf("DENIED\n");

                    conn.pop_read(read_id);
                    continue;
                }

                found_auth = auth_manage.fetch(read_id);

                if(found_auth.has_value() && proto.type == network_mode::STEAM_AUTH)
                {

                }

                if(found_auth.has_value() && !found_auth.value()->data.default_init)
                {
                    found_auth.value()->data.default_init = true;

                    {
                        db_read_write tx(get_db(), AUTH_DB_ID);

                        found_auth.value()->save(tx);
                    }
                }

                if(proto.type == network_mode::DATA)
                {
                    network_json = std::move(proto.data);
                }

                conn.pop_read(read_id);
            }

            client_input read_data = deserialise<client_input>(network_json);
        }


        auto clients = conn.clients();

        for(auto& i : clients)
        {
            auto found_auth = auth_manage.fetch(i);

            if(!found_auth.has_value())
                continue;

            if(!found_auth.value()->data.default_init)
                continue;

            server_data& next_data = data_model.fetch_by_id(i);

            ///write stuff to clients

            if(data_model.backup.find(i) != data_model.backup.end())
            {
                sf::Clock total_encode;

                nlohmann::json ret = serialise_against(next_data, data_model.backup[i], true, stagger_id);

                std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

                write_data dat;
                dat.id = i;
                dat.data = std::string(cb.begin(), cb.end());

                conn.write_to(dat);

                ///basically clones model, by applying the current diff to last model
                ///LEAKS MEMORY ON POINTERS
                deserialise(ret, data_model.backup[i]);
            }
            else
            {
                nlohmann::json ret = serialise(next_data);

                std::vector<uint8_t> cb = nlohmann::json::to_cbor(ret);

                write_data dat;
                dat.id = i;
                dat.data = std::string(cb.begin(), cb.end());

                conn.write_to(dat);

                data_model.backup[i] = server_data();
                serialisable_clone(next_data, data_model.backup[i]);
            }
        }

        stagger_id++;

        sf::sleep(sf::milliseconds(8));
    }
}

int main()
{
    std::thread(server_thread).detach();

    sf::ContextSettings sett;
    sett.antialiasingLevel = 8;
    sett.sRgbCapable = true;
    sett.majorVersion = 3;
    sett.minorVersion = 3;

    sf::RenderWindow window(sf::VideoMode(1600, 900), "hi", sf::Style::Default, sett);

    camera render_cam({1600, 900});

    sf::Texture font_atlas;

    ImGui::SFML::Init(window, false);

    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.Fonts->AddFontFromFileTTF("VeraMono.ttf", 14.f);

    ImFontAtlas* atlas = ImGui::SFML::GetFontAtlas();

    auto write_data =  [](unsigned char* data, sf::Texture& tex_id, int width, int height)
    {
        tex_id.create(width, height);
        tex_id.update((const unsigned char*)data);
    };

    ImGuiFreeType::BuildFontAtlas(atlas, ImGuiFreeType::ForceAutoHint, ImGuiFreeType::LEGACY);

    write_data((unsigned char*)atlas->TexPixelsNewRGBA32, font_atlas, atlas->TexWidth, atlas->TexHeight);

    atlas->TexID = (void*)font_atlas.getNativeHandle();

    ImGuiStyle& style = ImGui::GetStyle();

    style.FrameRounding = 0;
    style.WindowRounding = 0;
    style.FrameBorderSize = 0;

    ImGui::SetStyleLinearColor(sett.sRgbCapable);

    connection conn;
    #ifndef CLIENT_ONLY
    conn.connect("192.168.0.54", 11000);
    #else
    conn.connect("77.97.17.179", 11000);
    #endif // CLIENT_ONLY

    server_data model;

    sf::Clock imgui_delta;
    sf::Clock frametime_delta;

    sf::Keyboard key;
    sf::Mouse mouse;

    entity_manager entities;
    entities.use_aggregates = false;

    entity* some_default_entity = entities.make_new<entity>();

    some_default_entity->r.init_rectangular({20, 20});
    some_default_entity->r.position = {100, 100};

    sf::Clock read_clock;

    steamapi api;

    if(!api.enabled)
        throw std::runtime_error("No steam");

    {
        api.request_auth_token("");

        while(!api.auth_success())
        {
            api.pump_callbacks();
        }

        std::vector<uint8_t> vec = api.get_encrypted_token();

        std::string str(vec.begin(), vec.end());

        network_protocol proto;
        proto.type = network_mode::STEAM_AUTH;
        proto.data = binary_to_hex(str, false);

        conn.writes_to(proto, -1);
    }

    bool focus = true;

    while(window.isOpen())
    {
        double frametime_dt = (frametime_delta.restart().asMicroseconds() / 1000.) / 1000.;

        sf::Event event;

        float mouse_delta = 0;

        while(window.pollEvent(event))
        {
            ImGui::SFML::ProcessEvent(event);

            if(event.type == sf::Event::Closed)
            {
                exit(0);
            }

            if(event.type == sf::Event::Resized)
            {
                window.create(sf::VideoMode(event.size.width, event.size.height), "hi", sf::Style::Default, sett);
            }

            if(event.type == sf::Event::MouseWheelScrolled)
            {
                mouse_delta += event.mouseWheelScroll.delta;
            }

            if(event.type == sf::Event::LostFocus)
            {
                focus = false;
            }

            if(event.type == sf::Event::GainedFocus)
            {
                focus = true;
            }
        }

        if(ImGui::IsAnyWindowHovered())
        {
            mouse_delta = 0;
        }

        ImGui::SFML::Update(window,  imgui_delta.restart());

        entities.cleanup();
        entities.tick(frametime_dt);

        render_cam.screen_size = {window.getSize().x, window.getSize().y};
        render_cam.add_linear_zoom(mouse_delta);

        while(conn.has_read())
        {
            sf::Clock rtime;

            uint64_t rdata_id = conn.reads_from<server_data>(model);
            conn.pop_read(rdata_id);

            update_interpolated_variables(model);

            ///model contains latest server data
        }

        ImGui::Begin("Test Window");

        ImGui::End();

        /*
        network_protocol nproto;
        nproto.type = network_mode::DATA;

        client_input cinput;
        ///fill out client input



        nproto.data = serialise(cinput);
        nproto.rpcs = get_global_serialise_info();
        get_global_serialise_info().all_rpcs.clear();

        conn.writes_to(nproto, -1);*/


        ImGui::SFML::Render(window);

        entities.render(render_cam, window);

        window.display();
        window.clear();

        sf::sleep(sf::milliseconds(4));
    }


    return 0;
}
