#include <iostream>
#include <imgui/imgui.h>
#include <imgui-sfml/imgui-SFML.h>
#include <imgui/misc/freetype/imgui_freetype.h>
#include <SFML/Graphics.hpp>
#include <nauth/auth.hpp>
#include <nauth/steam_auth.hpp>
#include <nauth/steam_api.hpp>
#include <networking/networking.hpp>

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

void server_thread()
{
    set_db_location("./db");
    set_num_dbs(1);

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
}

int main()
{

    return 0;
}
