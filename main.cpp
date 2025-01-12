#include <dpp/dpp.h>
#include <stdlib.h>

int main() {
    dpp::cluster bot(getenv("QURAN_BOT_TOKEN"));
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([&bot](const auto& event) {
        if (dpp::run_once<struct ClearBotCommands>()) {
            bot.global_bulk_command_delete();
        }

        bot.global_bulk_command_create({dpp::slashcommand("quote", "Quote the Holy Qur'an", bot.me.id)
                .set_interaction_contexts({dpp::itc_guild, dpp::itc_bot_dm, dpp::itc_private_channel})});
    });

    bot.register_command("quote", [](const dpp::slashcommand_t& e) {
        e.reply("This is the `/quote` command." + std::string(
                                                      e.command.is_user_app_interaction() ? " Executing as a user interaction owned by user: <@" + e.command.get_authorizing_integration_owner(dpp::ait_user_install).str() + ">" : " Executing as a guild interaction on guild id " + e.command.guild_id.str()));
    });

    bot.start(dpp::st_wait);
}