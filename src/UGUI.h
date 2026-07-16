#ifndef UGUI_H
#define UGUI_H
#include <optional>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuiUtils.h"

typedef struct PositionOffset {
	float x;
	float y;
} PositionOffset;

class UGWindow {
public:
	UGWindow(const std::string &title, int flags = 0) : title(new std::string(title)), flags(flags) {
		showing = false;
		SetAlpha();
		SetMinSize();
		SetMaxSize();
	}
	~UGWindow() {
		delete title;
	}

	void Show() {
		showing = true;
	}
	void Hide() {
		showing = false;
	}
	boolean IsShowing() {
		return showing;
	}
	void draw(bool override = false) {
		if (override || showing) {
			if (corner != -1)
			{
				ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImVec2 window_pos = ImVec2((corner & 1) ? (viewport->Pos.x + viewport->Size.x - offsetX) : (viewport->Pos.x + offsetX), (corner & 2) ? (viewport->Pos.y + viewport->Size.y - offsetY) : (viewport->Pos.y + offsetY));
				ImVec2 window_pos_pivot = ImVec2((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
				ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
				ImGui::SetNextWindowViewport(viewport->ID);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));  // Initial white color for text

			ImGui::SetNextWindowSizeConstraints(minSize, maxSize);
			ImGui::SetNextWindowBgAlpha(alpha);
			ImGui::Begin(title->c_str(), &showing, flags);
			Render();
			ImGui::End();
			ImGui::PopStyleColor();
		}
	}
	
	void SetPosition(const std::string& position, float offsetX = 10.0f, float offsetY = 10.0f) {
		this->offsetX = offsetX;
		this->offsetY = offsetY;

		if (position == "top-left") {
			this->corner = 0;
		}
		else if (position == "top-right") {
			this->corner = 1;
		}
		else if (position == "bottom-left") {
			this->corner = 2;
		}
		else if (position == "bottom-right") {
			this->corner = 3;
		}
		else {
			this->corner = -1;
		}
	}

	void SetAlpha(float alpha = 0.85f) {
		this->alpha = alpha;
	}

	void SetMinSize(float x = 250.0f, float y = 50.0f) {
		minSize = ImVec2(x, y);
	}

	void SetMaxSize(float x = FLT_MAX, float y = FLT_MAX) {
		maxSize = ImVec2(x, y);
	}
	

protected:
	virtual void Render() = 0;
	void CreateTitle(const std::string& title, ImFont *font = nullptr) {
		if (font != nullptr) {
			ImGui::PushFont(font);
		}

		ImGui::TextColored(ImColor(52, 152, 219), title.c_str());
		ImGui::PopFont();
		ImGui::Separator();
	}

	void CreateKeyValueLabel(const std::string& key, const std::string& value, ImColor color = ImColor(200,200,200)) {
		ImGui::Text(key.c_str());
		ImGui::SameLine(0, 4.0f);
		ImGui::TextColored(color, value.c_str());
	}

	void CreateLabel(const std::string& label, std::optional<ImColor> color = std::nullopt) {
		if (color.has_value()) {
			ImGui::TextColored(color.value(), label.c_str());
		}
		else {
			ImGui::Text(label.c_str());
		}


	}
	
private:
	int corner;
	int flags;
	float offsetX;
	float offsetY;
	float alpha;
	ImVec2 minSize;
	ImVec2 maxSize;

	bool showing;
	std::string* title = nullptr;
};



#endif //UGUI_H