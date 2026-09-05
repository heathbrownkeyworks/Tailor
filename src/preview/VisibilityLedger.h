#pragma once

#include <unordered_map>
#include <utility>

namespace Tailor::Preview
{
    // Keep exact scene objects alive until their owned visibility change is
    // restored. Reference handles cannot identify both player skeletons.
    template<class StrongPointer>
    class VisibilityLedger
    {
        using Pointer = decltype(std::declval<StrongPointer>().get());
    public:
        bool Owns(Pointer node) const { return _hidden.contains(node); }
        bool Hide(Pointer node)
        {
            if (!node || node->GetAppCulled()) return false;
            _hidden.try_emplace(node, node);
            node->SetAppCulled(true);
            return true;
        }
        void Release(Pointer node)
        {
            if (auto it = _hidden.find(node); it != _hidden.end()) {
                node->SetAppCulled(false);
                _hidden.erase(it);
            }
        }
        template<class Keep> void Reassert(Keep keep)
        {
            for (auto it = _hidden.begin(); it != _hidden.end();) {
                auto* node = it->first;
                if (keep(node)) {
                    node->SetAppCulled(false);
                    it = _hidden.erase(it);
                } else {
                    node->SetAppCulled(true);
                    ++it;
                }
            }
        }
        void Restore()
        {
            for (auto& [node, hold] : _hidden) node->SetAppCulled(false);
            _hidden.clear();
        }
        std::size_t Size() const { return _hidden.size(); }
    private:
        std::unordered_map<Pointer, StrongPointer> _hidden;
    };
}
