#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace Tailor::Wigs
{
    enum class RecoveryAction { Skip, EquipExisting, AddAndEquip };

    // WigManager's recursive mutex protects this state. Tokens never reset across
    // game loads, so a sleeping worker cannot act on a new assignment or save.
    class WigRecoveryPolicy
    {
    public:
        struct Request
        {
            std::uint32_t actorId;
            std::uint32_t wigFormId;
            std::uint64_t token;
        };

        class Change
        {
        public:
            Change(WigRecoveryPolicy& policy, std::uint32_t actorId) : _policy(policy), _actorId(actorId)
            {
                ++_policy._changes[_actorId];
                _policy._pending.erase(_actorId);
            }
            ~Change()
            {
                if (--_policy._changes.at(_actorId) == 0) _policy._changes.erase(_actorId);
            }
            Change(const Change&) = delete;
            Change& operator=(const Change&) = delete;

        private:
            WigRecoveryPolicy& _policy;
            std::uint32_t _actorId;
        };

        [[nodiscard]] Change BeginChange(std::uint32_t actorId) { return Change(*this, actorId); }

        void SetEnabled(bool enabled)
        {
            _enabled = enabled;
            if (!enabled) _pending.clear();
        }

        std::optional<Request> Queue(std::uint32_t actorId, std::uint32_t wigFormId)
        {
            if (!_enabled || actorId == 0 || wigFormId == 0 ||
                _changes.contains(actorId) || _pending.contains(actorId)) return std::nullopt;
            const Request request{actorId, wigFormId, ++_nextToken};
            _pending.emplace(actorId, request);
            return request;
        }

        bool IsCurrent(const Request& request) const
        {
            const auto it = _pending.find(request.actorId);
            return _enabled && it != _pending.end() && it->second.token == request.token;
        }

        void Finish(const Request& request)
        {
            if (IsCurrent(request)) _pending.erase(request.actorId);
        }

        static RecoveryAction Decide(bool sameAssignment, bool actorReady, bool previewing,
            bool equipped, std::int32_t inventoryCount)
        {
            if (!sameAssignment || !actorReady || previewing || equipped) return RecoveryAction::Skip;
            return inventoryCount > 0 ? RecoveryAction::EquipExisting : RecoveryAction::AddAndEquip;
        }

    private:
        bool _enabled{false};
        std::uint64_t _nextToken{0};
        std::unordered_map<std::uint32_t, Request> _pending;
        std::unordered_map<std::uint32_t, unsigned> _changes;
    };
}
