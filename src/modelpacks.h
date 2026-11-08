#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Model packs on the Mods page (aki-roadmap#130): the aki_modelimport packs in
// <mods>\models\ (<id>.akimodel folders made by AkiTools aki-model), and
// models.txt, which says who each pack replaces. The game's mod reads both at
// every match start; the launcher only edits the files.
//
// models.txt (the runtime contract): one line per rule, '#' starts a comment
//   <pack> slot N                    a wrestler slot of the match (1-4)
//   <pack> wrestler 0xNNNN [costume C]
//   off <pack>                       the pack is off, whatever else says
// The first matching line wins. Lines the launcher doesn't understand are kept
// as they are.

struct ModelPack {
    std::filesystem::path dir;  // <mods>\models\<id>.akimodel
    std::string id;             // the folder name without .akimodel
    std::string name, author, license, source;  // pack.json (UTF-8; empty when missing)
    std::filesystem::path preview;              // preview.png, when there is one
    int triangles = 0;                          // manifest.json
};
std::vector<ModelPack> list_model_packs(const std::filesystem::path& modelsDir);
// The runtime's pack-id rule: a-z 0-9 _ -, at most 31 characters. A folder
// named otherwise can't be named in models.txt (the launcher leaves it alone).
bool valid_pack_id(const std::string& id);

struct ModelsTxtLine {
    enum class Kind { Blank, Comment, Slot, Wrestler, Off, Other };
    Kind kind = Kind::Other;
    std::string raw;        // the line as read (no line end); rewritten lines are regenerated
    std::string pack;       // Slot / Wrestler / Off
    int slot = 0;           // Slot
    int wrestler = -1;      // Wrestler (the id)
    int costume = -1;       // Wrestler, -1 = any
    std::string comment;    // a trailing "# ..." (kept when the line is rewritten)
};

struct ModelsTxt {
    std::vector<ModelsTxtLine> lines;
    std::string newline = "\r\n";  // the file's own line ends are kept
    bool bom = false;

    static ModelsTxt parse(const std::string& text);
    std::string serialize() const;

    // The pack's assignment (its first slot or wrestler line), or nullptr.
    const ModelsTxtLine* assignment(const std::string& pack) const;
    int assignment_count(const std::string& pack) const;  // its slot + wrestler lines
    bool is_off(const std::string& pack) const;
    // Replaces the pack's slot / wrestler lines with one, first among the
    // assignments so it wins (kind Blank = no assignment: just removes them).
    void set_assignment(const std::string& pack, ModelsTxtLine::Kind kind, int slot, int wrestler, int costume,
                        const std::string& comment = "");
    void set_off(const std::string& pack, bool off);
    // Packs models.txt names that aren't in `installed` (for a "not installed" note).
    std::vector<std::string> missing_packs(const std::vector<ModelPack>& installed) const;
};

// models.txt from disk ('' when there is none); writes go through a .new file
// and an atomic replace, so the game never reads half a file.
bool read_models_txt(const std::filesystem::path& file, ModelsTxt* out, std::wstring* err);
bool write_models_txt(const std::filesystem::path& file, const ModelsTxt& txt, std::wstring* err);

// A game's wrestler roster (AkiTools <game>-roster.json, aki-roadmap#147),
// for picking a wrestler by name.
struct RosterEntry {
    int id = 0;
    std::string name;  // UTF-8
    std::vector<std::pair<int, std::string>> costumes;
};
struct Roster {
    std::vector<RosterEntry> wrestlers;
    const RosterEntry* find(int id) const;
};
bool load_roster_file(const std::filesystem::path& file, Roster* out, std::wstring* err);
// Where the launcher looks: assets\rosters\<profile>-roster.json next to it,
// then the depot's AkiTools\model-templates\profiles\<profile>-roster.json.
std::filesystem::path find_roster(const std::string& profile, const std::filesystem::path& appRoot,
                                  const std::filesystem::path& depotRoot);
