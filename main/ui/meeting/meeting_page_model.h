#pragma once

#include <string>
#include <vector>

namespace ui::meeting {

struct MeetingRowModel {
    int id = 0;
    std::string title;
    std::string status;
    std::string summary_preview;
};

struct MeetingPageModel {
    std::string title = "会议纪要";
    std::vector<MeetingRowModel> meetings;
    std::string status_line;
    bool loading = false;
    bool show_detail = false;
    int selected_id = 0;
    std::string detail_title;
    std::string detail_status;
    std::string detail_body;
};

}  // namespace ui::meeting
