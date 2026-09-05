#pragma once

#include <optional>

namespace Tailor
{
    // Scope Skyrim's TM visibility flag to the Tailor UI, including opens with
    // no preview target. Meridian renders its browser and cursor separately.
    class GameMenuVisibility
    {
    public:
        template <class UI>
        void Hide(UI* ui)
        {
            if (!ui || _wasShowing.has_value()) return;
            _wasShowing = ui->IsShowingMenus();
            if (*_wasShowing) ui->ShowMenus(false);
        }

        template <class UI>
        void Restore(UI* ui)
        {
            if (!ui || !_wasShowing.has_value()) return;
            if (*_wasShowing && !ui->IsShowingMenus()) ui->ShowMenus(true);
            _wasShowing.reset();
        }

    private:
        std::optional<bool> _wasShowing;
    };
}
