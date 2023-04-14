// todo: separate files, create makefile
// figure out const for some of the static embeds?
#include <dpp/dpp.h>
#include <dpp/nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <set>
#include <iterator>
#include <algorithm>
#include <fstream>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

struct Player {
    const dpp::snowflake id;
    bool alive; 
    bool answered;
    Player(dpp::snowflake id) : id(id), alive(true), answered(false) { }
};

struct Answer {
    bool picked;
    bool correct;
    Answer() { }
    Answer(bool correct) : picked(false), correct(correct) { }
};

class TriviaGame {
    public:
    const dpp::snowflake channel_id;
    dpp::snowflake recent_embed_id;

    int question_number;
    int alive_player_count;
    
    static dpp::cluster* bot;
    static std::unordered_map<dpp::snowflake, TriviaGame>* game_map;
    static json* questions_json;

    std::string question_text;
    std::unordered_map<std::string, Answer> answers;
    std::vector<Player> players;
    Player* answering_player;
    std::set<dpp::timer> countdowns;

    TriviaGame();
    TriviaGame(dpp::snowflake channel_id);

    dpp::message start_lobby();
    void add_player(const dpp::button_click_t& event);
    void send_question();
    dpp::snowflake send_dropdown();
    dpp::component dropdown_menu();
    void next_player();
    void set_time_limit(dpp::snowflake prompt_id, int time_limit);
    void get_new_question();
    void next_question();
    void send_finalized_question_embed(bool all_correct);
    bool check_answer(const std::string& answer);
    bool validate_menu(const std::string& menu_text);
    bool all_players_correct();
    void congratulate_winner();
    void stop_game();
};

TriviaGame::TriviaGame() { }
TriviaGame::TriviaGame(dpp::snowflake channel_id) : channel_id(channel_id) {
    question_number = -1; 
    alive_player_count = 0;
}

dpp::cluster* TriviaGame::bot;
std::unordered_map<dpp::snowflake, TriviaGame>* TriviaGame::game_map;
json* TriviaGame::questions_json;

dpp::message TriviaGame::start_lobby() {
    dpp::embed lobby_embed = dpp::embed()
        .set_color(dpp::colors::sti_blue)
        .set_title("Process of Elimination")
        .set_description("Game will begin in 20 seconds!\n\nRules: A multiple choice question will be presented - however, only 1 answer is wrong. Players are randomly chosen one at a time to answer the question. Once an answer is selected, it cannot be used by other answerers. Picking the wrong answer results in elimination. The time limit to pick an answer is 20 seconds.\n\nLast player standing wins! Good luck!");
    
    dpp::component join_button = dpp::component()
        .add_component(dpp::component()
        .set_type(dpp::cot_button)
        .set_label("Join game")
        .set_emoji("👍")
        .set_id("join")
    );

    bool warning_given = false;
    dpp::embed warning_embed = dpp::embed()
        .set_color(dpp::colors::sti_blue)
        .set_title("Game starts in 10 seconds!");
    
    auto channel_id_ = channel_id;
    auto game_map_ = game_map;
    dpp::timer timer_handle = bot->start_timer([=](dpp::timer timer_handle) {
        if (!game_map_->count(channel_id_)) {
            return;
        } else if (!countdowns.count(timer_handle)) {
            return;
        }

        if (question_number == -1) {
            bot->message_create(dpp::message(channel_id, warning_embed));
            question_number = 0;
        } else if (players.size() > 1) {
            countdowns.erase(timer_handle);
            next_question();
            bot->stop_timer(timer_handle);
        } else {
            dpp::embed lacking_players_message = dpp::embed()
                .set_color(dpp::colors::red)
                .set_title("Not enough players have joined. The game has been cancelled.");
            bot->message_create(dpp::message(channel_id, lacking_players_message));
            stop_game();
            bot->stop_timer(timer_handle);
        }
    }, 10);
    countdowns.insert(timer_handle);

    dpp::message lobby_message(channel_id, lobby_embed);
    lobby_message.add_component(join_button);
    return lobby_message;
}

void TriviaGame::add_player(const dpp::button_click_t& event) {
    dpp::message join_message(channel_id, "");
    auto player_id = event.command.usr.id;

    bool already_joined = false;
    for (auto player : players) {
        if (player.id == player_id) {
            already_joined = true;
        }
    }
    if (question_number > 0) {
        join_message
            .set_content("⚠️ A game is in progress!")
            .set_flags(dpp::message_flags::m_ephemeral);
    } else if (already_joined) {
        join_message
            .set_content("⚠️ You have already joined!")
            .set_flags(dpp::message_flags::m_ephemeral);
    } else if (players.size() >= 8) {
        join_message
            .set_content("⚠️ Player maximum has been reached!")
            .set_flags(dpp::message_flags::m_ephemeral);
    } else {
        players.push_back(Player(player_id));
        alive_player_count++;

        dpp::embed join_embed = dpp::embed()
            .set_color(dpp::colors::sti_blue)
            .set_description("<@" + std::to_string(player_id) + "> has joined the game! (" + std::to_string(players.size()) + "/8)");
        join_message.add_embed(join_embed);
        bot->message_create(join_message);
        event.reply();
        return;
    }
    event.reply(join_message);
}

void TriviaGame::send_question() {
    dpp::embed question_embed = dpp::embed()
        .set_color(dpp::colors::sti_blue)
        .set_title("Question " + std::to_string(question_number) + ": " + question_text);
    std::string answer_text;
    std::string player_text;

    for (auto &answer : answers) {
        if (answer.second.picked) {
            answer_text += "☑️ ~~" + answer.first + "~~\n";
        } else {
            answer_text += "🎁 " + answer.first + "\n";
        }
    }
    answer_text += "\n";

    for (auto &player : players) {
        std::string player_status_emoji;
        if (!player.alive) {
            player_text += "💀 ~~<@" + std::to_string(player.id) + ">~~\n";
            continue;
        } else if (player.answered) {
            player_status_emoji = "✅";
        } else if (&player == answering_player) {
            player_status_emoji = "🤔";
        } else {
            player_status_emoji = "❓";
        }
        player_text += player_status_emoji + " <@" + std::to_string(player.id) + ">\n";
    }

    question_embed.add_field("Answers:", answer_text, false);
    question_embed.add_field("Players:", player_text, false);
    recent_embed_id = bot->message_create_sync(dpp::message(channel_id, question_embed)).id;
}

dpp::snowflake TriviaGame::send_dropdown() {
    dpp::message prompt = dpp::message(channel_id, "<@" + std::to_string(answering_player->id) + "> Your turn to answer!");
    prompt
        .set_allowed_mentions(false, false, false, false, {answering_player->id}, {})
        .add_component(dropdown_menu());
    return bot->message_create_sync(prompt).id;
}

dpp::component TriviaGame::dropdown_menu() {
    dpp::component answer_dropdown = dpp::component()
        .add_component(dpp::component()
        .set_type(dpp::cot_selectmenu)
        .set_placeholder("Select an answer"));

    for (auto &answer : answers) {
        if (!answer.second.picked) {
            answer_dropdown.components[0]
            .add_select_option(dpp::select_option(answer.first, answer.first, "")
            .set_emoji(u8"🎁"));
        }
    }

    return answer_dropdown;
}

void TriviaGame::next_player() {
    do 
        answering_player = &players[rand() % players.size()];
    while (!answering_player->alive || answering_player->answered);

    send_question();
    dpp::snowflake prompt_id = send_dropdown();
    set_time_limit(prompt_id, 20);
}

void TriviaGame::set_time_limit(dpp::snowflake prompt_id, int time_limit) {
    auto timed_player = answering_player;
    auto timed_question = question_number;

    auto channel_id_ = channel_id;
    auto game_map_ = game_map;
    dpp::timer timer_handle = bot->start_timer([=](dpp::timer timer_handle) {
        if (!game_map_->count(channel_id_)) {
            return;
        } else if (!countdowns.count(timer_handle)) {
            return;
        }
        
        countdowns.erase(timer_handle);
        if (!timed_player->answered && timed_question == question_number) {
            bot->message_delete(recent_embed_id, channel_id);
            bot->message_delete(prompt_id, channel_id);
            timed_player->alive = false;
            alive_player_count--;
            dpp::embed out_of_time_embed = dpp::embed()
                .set_color(dpp::colors::red)
                .set_description("**💥💥 <@" + std::to_string(timed_player->id) + "> ran out of time!💥💥**");  
            bot->message_create(dpp::message(channel_id, out_of_time_embed));
            sleep(3);
            send_finalized_question_embed(false);
            sleep(3);
            if (alive_player_count > 1) {
                next_question();
            } else {
                congratulate_winner();
            }
            bot->stop_timer(timer_handle);
        }
    }, time_limit);
    countdowns.insert(timer_handle);
}

void TriviaGame::get_new_question() {
    auto& question_info = (*questions_json)["question_list"][rand() % (*questions_json)["question_list"].size()];
    int wrong_answer_index = rand() % (alive_player_count + 1);

    question_text = question_info["question"];
    int random_index;
    for (int i = 0; i <= alive_player_count; i++) {
        if (i == wrong_answer_index) {
            answers.emplace(question_info["wrong"], Answer(false));
        } else {
            do {
                random_index = rand() % 8;
            } while (answers.count(question_info["correct"][random_index]));
            answers.emplace(question_info["correct"][random_index], Answer(true));
        }
    }
}

void TriviaGame::next_question() {
    answers.clear();
    for (auto& player : players) {
        player.answered = false;
    }
    question_number++;

    get_new_question();
    next_player();
}

void TriviaGame::send_finalized_question_embed(bool all_correct) {
    dpp::embed question_embed = dpp::embed()
        .set_title("Question " + std::to_string(question_number) + ": " + question_text);
    
    std::string answer_text;
    std::string player_text;
    std::string wrong_answer_emoji;
    
    if (all_correct) {
        question_embed.set_color(dpp::colors::light_sea_green);
        wrong_answer_emoji = "❌";
    } else {
        question_embed.set_color(dpp::colors::red);
        wrong_answer_emoji = "💥";
    }

    for (auto &answer : answers) {
        if (!answer.second.correct) {
            answer_text += wrong_answer_emoji + " **" + answer.first + "**\n";
        } else if (answer.second.picked) {
            answer_text += "☑️ ~~" + answer.first + "~~\n";
        } else {
            answer_text += "🎁 " + answer.first + "\n";
        }
    }

    for (auto &player : players) {
        if (!player.alive) {
            player_text += "💀 ~~<@" + std::to_string(player.id) + ">~~\n";
        } else {
            player_text += "✅ <@" + std::to_string(player.id) + ">\n";
        }
    }

    question_embed.add_field("Answers:", answer_text, false);
    question_embed.add_field("Players:", player_text, false);
    bot->message_create(dpp::message(channel_id, question_embed));
    
    if (all_correct) {
        sleep(3);
        dpp::embed all_players_correct_embed = dpp::embed()
            .set_color(dpp::colors::light_sea_green)
            .set_description("**😃 Everyone got the question right!**");
        bot->message_create(dpp::message(channel_id, all_players_correct_embed));
    }
    sleep(3);
}

bool TriviaGame::check_answer(const std::string& answer) {
    answering_player->answered = true;
    std::string reveal_text = "<@" + std::to_string(answering_player->id) + "> chose **" + answer + "**, which is...";
    dpp::embed reveal_embed = dpp::embed()
        .set_color(dpp::colors::sti_blue)
        .set_description(reveal_text);
    dpp::snowflake embed_id = bot->message_create_sync(dpp::message(channel_id, reveal_embed)).id;
    sleep(3);

    if (answers[answer].correct) {
        answers[answer].picked = true;
        reveal_embed.description += "\n\n✅ **Correct!** ✅";
        reveal_embed.set_color(dpp::colors::light_sea_green);
    } else {
        answering_player->alive = false;
        alive_player_count--;
        reveal_embed.description += "\n\n💥💥 **INCORRECT!!!** 💥💥";
        reveal_embed.set_color(dpp::colors::red);
    }
    dpp::message updated_message(channel_id, reveal_embed);
    updated_message.id = embed_id;
    bot->message_edit(updated_message);
    sleep(3);
    return answers[answer].correct;
}

bool TriviaGame::validate_menu(const std::string& menu_text) {
    for (auto& answer : answers) {
        if (answer.first == menu_text)
            return true;
    }
    return false;
}

bool TriviaGame::all_players_correct() {
    for (auto& player : players) {
        if (!player.answered && player.alive)
            return false;
    }
    return true;
}

void TriviaGame::congratulate_winner() {
    dpp::snowflake winner_id(0);
    for (auto& player : players) {
        if (player.alive)
            winner_id = player.id;
    }

    dpp::message winner_message = dpp::message(channel_id, "**🥳 Congratulations <@" + std::to_string(winner_id) + ">! You are the winner!**")
        .set_allowed_mentions(false, false, false, false, {winner_id}, {});

    bot->message_create(winner_message);
    stop_game();
}

void TriviaGame::stop_game() {;
    for (auto countdown : countdowns) {
        bot->stop_timer(countdown);
    }
    game_map->erase(channel_id);
}

int main() {
    std::ifstream questions_file("questions.json");
    json questions_json;
    questions_file >> questions_json;

    srand(time(NULL));
    std::unordered_map<dpp::snowflake, TriviaGame> game_map; 
    dpp::cluster bot("");
    bot.on_log(dpp::utility::cout_logger());

    TriviaGame::bot            = &bot;
    TriviaGame::game_map       = &game_map;
    TriviaGame::questions_json = &questions_json;

    bot.on_ready([&bot](const dpp::ready_t& event) {
        if (dpp::run_once<struct create_slash_commands>()) {
            bot.global_command_create(dpp::slashcommand("start", "Start a new game", bot.me.id));
            bot.global_command_create(dpp::slashcommand("stop", "Stops currently running game", bot.me.id));
        }
    });
 
    bot.on_slashcommand([&bot, &game_map, &questions_json](const dpp::slashcommand_t& event){
        auto channel_id = event.command.channel_id;
        if (event.command.get_command_name() == "start") {
            if (!game_map.count(channel_id)) {
                game_map.emplace(channel_id, TriviaGame(channel_id));
                event.reply(game_map[channel_id].start_lobby());
            } else {
                event.reply(
                    dpp::message(channel_id, "⚠️ A game is already running in this channel!")
                        .set_flags(dpp::message_flags::m_ephemeral)
                );
            }
        } else if (event.command.get_command_name() == "stop") {
            auto channel_id = event.command.channel_id;

            if (game_map.count(channel_id)) {
                game_map[channel_id].stop_game();
                event.reply("🛑 Game has been stopped.");
            } else {
                event.reply(
                    dpp::message(channel_id, "⚠️ No game is running in this channel!")
                        .set_flags(dpp::message_flags::m_ephemeral)
                );
            }
        }
    });       

    bot.on_select_click([&bot, &game_map](const dpp::select_click_t& event) {
        auto channel_id = event.command.channel_id;
        if (!game_map.count(channel_id)) {
            event.reply(
                dpp::message(channel_id, "⚠️ This game has stopped running!")
                    .set_flags(dpp::message_flags::m_ephemeral)
            );
        } else if (!game_map[channel_id].validate_menu(event.values[0])) {
            event.reply(
                dpp::message(channel_id, "⚠️ This game has stopped running!")
                    .set_flags(dpp::message_flags::m_ephemeral)
            );
        } else if (game_map[channel_id].answering_player->id != event.command.usr.id) {
            event.reply(
                dpp::message(channel_id, "⛔ It is not your turn to answer!")
                    .set_flags(dpp::message_flags::m_ephemeral)
            );
        } else {
            bot.message_delete(event.command.message_id, channel_id);
            event.reply();
            bool correct_answer = game_map[channel_id].check_answer(event.values[0]);
            bot.message_delete(game_map[channel_id].recent_embed_id, channel_id);
            
            if (!correct_answer) {
                game_map[channel_id].send_finalized_question_embed(false);
                if (game_map[channel_id].alive_player_count > 1) {
                    game_map[channel_id].next_question();
                } else {
                    game_map[channel_id].congratulate_winner();
                }
            } else if (game_map[channel_id].all_players_correct()) {
                game_map[channel_id].send_finalized_question_embed(true);
                game_map[channel_id].next_question();
            } else {
                sleep(3);
                game_map[channel_id].next_player();
            }
        }
    });

    bot.on_button_click([&bot, &game_map](const dpp::button_click_t& event) {
        auto channel_id = event.command.channel_id;
        if (game_map.count(channel_id)) {
            game_map[channel_id].add_player(event);
        } else {
            event.reply(
                dpp::message(channel_id, "⚠️ No game is running in this channel!")
                    .set_flags(dpp::message_flags::m_ephemeral)
            );
        }
    });

    bot.start(dpp::st_wait);
 
    return 0;
}