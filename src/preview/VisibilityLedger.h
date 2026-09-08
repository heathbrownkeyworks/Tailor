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
            if (!node) return false;
            const bool wasHidden = node->GetAppCulled();
            // Off-camera branches can be revealed by Skyrim during an orbit.
            // Track them now and preserve their original visibility on close.
            _hidden.try_emplace(node, Entry{StrongPointer(node), wasHidden});
            node->SetAppCulled(true);
            return !wasHidden;
        }
        void Release(Pointer node)
        {
            if (auto it = _hidden.find(node); it != _hidden.end()) {
                node->SetAppCulled(it->second.wasHidden);
                _hidden.erase(it);
            }
        }
        template<class Keep> void Reassert(Keep keep)
        {
            for (auto it = _hidden.begin(); it != _hidden.end();) {
                auto* node = it->first;
                if (keep(node)) {
                    node->SetAppCulled(it->second.wasHidden);
                    it = _hidden.erase(it);
                } else {
                    node->SetAppCulled(true);
                    ++it;
                }
            }
        }
        void Restore()
        {
            for (auto& [node, entry] : _hidden) node->SetAppCulled(entry.wasHidden);
            _hidden.clear();
        }
        std::size_t Size() const { return _hidden.size(); }
    private:
        struct Entry { StrongPointer hold; bool wasHidden; };
        std::unordered_map<Pointer, Entry> _hidden;
    };
}
