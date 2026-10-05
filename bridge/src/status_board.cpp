#include "status_board.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bridge {

bool StatusBoard::add(std::string key, std::string group, std::string label, std::string unit, int decimals)
{
    StatusItem item;
    item.key      = std::move(key);
    item.group    = std::move(group);
    item.label    = std::move(label);
    item.unit     = std::move(unit);
    item.decimals = std::clamp(decimals, 0, 6);
    return addItem(std::move(item));
}

bool StatusBoard::addRanged(std::string key, std::string group, std::string label, std::string unit, double minimum,
                            double maximum, int decimals)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !(minimum < maximum))
    {
        return false;
    }
    StatusItem item;
    item.key      = std::move(key);
    item.group    = std::move(group);
    item.label    = std::move(label);
    item.unit     = std::move(unit);
    item.hasRange = true;
    item.minimum  = minimum;
    item.maximum  = maximum;
    item.decimals = std::clamp(decimals, 0, 6);
    return addItem(std::move(item));
}

bool StatusBoard::addItem(StatusItem item)
{
    if (item.key.empty())
    {
        return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_index.find(item.key) != m_index.end())
    {
        return false;
    }
    m_index.emplace(item.key, m_items.size());
    m_items.push_back(std::move(item));
    return true;
}

StatusItem* StatusBoard::find(std::string_view key)
{
    const auto found = m_index.find(key);
    return found == m_index.end() ? nullptr : &m_items[found->second];
}

bool StatusBoard::set(std::string_view key, double value)
{
    std::lock_guard lock(m_mutex);
    StatusItem*     item = find(key);
    if (!item)
    {
        return false;
    }
    item->kind   = StatusItem::Kind::Number;
    item->number = value;
    item->text.clear();
    m_published   = true;
    m_lastPublish = Clock::now();
    return true;
}

bool StatusBoard::setText(std::string_view key, std::string_view text)
{
    std::lock_guard lock(m_mutex);
    StatusItem*     item = find(key);
    if (!item)
    {
        return false;
    }
    item->kind = StatusItem::Kind::Text;
    item->text.assign(text);
    m_published   = true;
    m_lastPublish = Clock::now();
    return true;
}

StatusSnapshot StatusBoard::snapshot() const
{
    std::lock_guard lock(m_mutex);
    StatusSnapshot  result;
    result.items = m_items;
    if (m_published)
    {
        result.ageSeconds = std::chrono::duration<double>(Clock::now() - m_lastPublish).count();
    }
    return result;
}

} // namespace bridge
