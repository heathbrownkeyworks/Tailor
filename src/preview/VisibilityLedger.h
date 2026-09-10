#pragma once

#include <unordered_map>
#include <utility>

namespace Tailor::Preview
{
    // The player's third-person root can remain app-culled after a first-person
    // camera transition. Reveal only that root, never authored child partitions.
    template<class StrongPointer>
    class RootVisibilityOverride
    {
        using Pointer = decltype(std::declval<StrongPointer>().get());
    public:
        bool Show(Pointer node)
        {
            if (_root.get() != node) {
                Restore();
                _root = StrongPointer(node);
                _wasHidden = node && node->GetAppCulled();
            }
            if (!node || !node->GetAppCulled()) return false;
            node->SetAppCulled(false);
            return true;
        }
        void Restore()
        {
            // Restore our visible value only. Leave a subsequent external hide
            // alone, and hold the exact old root across equipment rebuilds.
            if (auto* node = _root.get(); node && !node->GetAppCulled()) {
                node->SetAppCulled(_wasHidden);
            }
            _root = StrongPointer(nullptr);
            _wasHidden = false;
        }
        bool WasHidden() const { return _wasHidden; }
    private:
        StrongPointer _root{nullptr};
        bool _wasHidden{false};
    };

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
