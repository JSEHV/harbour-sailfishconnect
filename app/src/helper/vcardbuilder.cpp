/*
 * Copyright 2019 Richard Liebscher <richard.liebscher@gmail.com>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "vcardbuilder.h"

#include <QMetaObject>
#include <QStringBuilder>
#include "cpphelper.h"

namespace SailfishConnect {

VCardBuilder::VCardBuilder()
{
    m_vCard.append(QStringLiteral("BEGIN:VCARD\nVERSION:4.0\n"));
}

void VCardBuilder::addRawProperty(const QString &name, const QString &rawValue)
{
    m_vCard.append(name % QChar(':') % rawValue % QChar('\n'));
}

QString VCardBuilder::result()
{
    return m_vCard + QStringLiteral("END:VCARD\n");
}

} // namespace SailfishConnect
