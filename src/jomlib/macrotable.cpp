/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of jom.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "macrotable.h"
#include "exception.h"

#include <QStringList>
#include <QRegExp>
#include <QDebug>

namespace NMakeFile {

// set this to some visible character if you're debugging filename macros
const QChar MacroTable::fileNameMacroMagicEscape = QChar::ByteOrderMark;

MacroTable::MacroTable()
{
}

MacroTable::~MacroTable()
{
}

QString MacroTable::macroValue(const QString& macroName) const
{
    return m_macros.value(macroName).value;
}

/**
 * Sets the value of a macro and marks it as environment variable.
 * That means changing the macro value changes the environment.
 * Note that environment macro names are converted to upper case.
 */
void MacroTable::defineEnvironmentMacroValue(const QString& name, const QString& value, bool readOnly)
{
    const QString upperName = name.toUpper();
    if (m_macros.contains(upperName)) {
        MacroData &md = m_macros[upperName];
        if (md.source == MacroSource::CommandLine) {
            md.source = MacroSource::Environment;
            setEnvironmentVariable(upperName, expandMacros(md.value));
        }
        return;
    }
    QString expandedValue;
    try {
        // The make variable gets the unexpanded value.
        // The environment variable gets the expanded value.
        expandedValue = expandMacros(value);
    } catch (const Exception &) {
        // Expanding the value caused an error. We don't create a Make variable for it.
        // See section "Environment-Variable Macros" in the nmake documentation.
        // Infamous example: PROMPT=$+$P$_$G
        return;
    }
    MacroData* macroData = internalSetMacroValue(upperName, value);
    if (!macroData)
        return;
    macroData->source = MacroSource::Environment;
    macroData->isReadOnly = readOnly;
    setEnvironmentVariable(upperName, expandedValue);
}

void MacroTable::defineCommandLineMacroValue(const QString &name, const QString &value)
{
    defineCommandLineMacroValueImpl(name, value, MacroSource::CommandLine);
}

void MacroTable::defineImplicitCommandLineMacroValue(const QString &name, const QString &value)
{
    defineCommandLineMacroValueImpl(name, value, MacroSource::CommandLineImplicit);
}

void MacroTable::defineCommandLineMacroValueImpl(const QString &name, const QString &value,
                                                 MacroSource source)
{
    MacroData* macroData = internalSetMacroValue(name, value, true);
    if (!macroData)
        return;
    macroData->source = source;
    macroData->isReadOnly = true;
}

bool MacroTable::isMacroNameValid(const QString& name) const
{
    static QRegExp rexMacroIdentifier;
    if (rexMacroIdentifier.isEmpty()) {
        rexMacroIdentifier.setPattern(QLatin1String("([A-Z]|_|)(\\w|\\.)+"));
        rexMacroIdentifier.setCaseSensitivity(Qt::CaseInsensitive);
    }

    return rexMacroIdentifier.exactMatch(name);
}

/**
 * Sets the value of a macro. If the macro doesn't exist, it is defines as
 * a normal macro (no environment variable) - changing the macro doesn't affect
 * the environment.
 * If the macros exists and is an environment variable then the corresponding
 * environment variable is set to the new macro value.
 */
void MacroTable::setMacroValue(const QString& name, const QString& value)
{
    setMacroValueImpl(name, value, MacroSource::MakeFile);
}

void MacroTable::setMacroValueImpl(const QString &name, const QString &value, MacroSource source)
{
    MacroData* macroData = internalSetMacroValue(name, value);
    if (!macroData) {
        QString msg = QLatin1String("macro name %1 is invalid");
        throw Exception(msg.arg(name));
    }

    macroData->source = source;
    if (macroData->source == MacroSource::Environment)
        setEnvironmentVariable(name, expandMacros(macroData->value));
}

void MacroTable::predefineValue(const QString &name, const QString &value)
{
    setMacroValueImpl(name, value, MacroSource::Predefinition);
}

/**
 * Sets the value of an environment variable.
 * The environment will be passed to the QProcess instances.
 */
void MacroTable::setEnvironmentVariable(const QString& name, const QString& value)
{
    m_environment[name] = value;
}

/**
 * String replace with lazy replacement evaluation.
 * getValue is only called if the search string is present.
 */
template <typename F>
void replaceStringWithLazyValue(QString &str, const QString &searchString, F getValue)
{
    int idx = str.indexOf(searchString);
    if (idx >= 0) {
        const QString oldValue = getValue();
        do {
            str.replace(idx, searchString.length(), oldValue);
            idx = str.indexOf(searchString, idx + oldValue.length());
        } while (idx >= 0);
    }
}

class MacroValueOp
{
public:
    MacroValueOp(MacroTable *mt, const QString &str)
        : mt(mt), str(str)
    {}

    QString operator()()
    {
        return mt->macroValue(str);
    }

private:
    const MacroTable *const mt;
    const QString &str;
};

MacroTable::MacroData* MacroTable::internalSetMacroValue(const QString &name, const QString &value,
                                                         bool ignoreReadOnly)
{
    QString expandedName = expandMacros(name);
    if (!isMacroNameValid(expandedName))
        return 0;

    MacroData* result = 0;
    const QString instantiatedName = QLatin1Literal("$(") + expandedName + QLatin1Literal(")");
    QString newValue = value;
    replaceStringWithLazyValue(newValue, instantiatedName, MacroValueOp(this, expandedName));

    result = &m_macros[expandedName];
    if (ignoreReadOnly || !result->isReadOnly)
        result->value = newValue;

    return result;
}

bool MacroTable::isMacroDefined(const QString& name) const
{
    return m_macros.contains(name);
}

void MacroTable::undefineMacro(const QString& name)
{
    m_macros.remove(name);
}

QString MacroTable::expandMacros(const QString& str, bool inDependentsLine) const
{
    QSet<QString> usedMacros;
    return expandMacros(str, inDependentsLine, usedMacros);
}

QString MacroTable::expandMacros(const QString& str, bool inDependentsLine, QSet<QString>& usedMacros) const
{
    QString ret;
    ret.reserve(str.count());

    int i = 0;
    int appendStart = -1, appendLength = 0;
    const int max_i = str.count() - 1;
    while (i <= max_i) {
        if (str.at(i) == QLatin1Char('$') && i < max_i) {
            if (appendStart != -1) {
                ret.append(str.mid(appendStart, appendLength));
                appendStart = -1;
            }

            ++i;
            if (str.at(i) == QLatin1Char('(')) {
                // found macro invocation
                int macroInvokationEnd = i+1;
                int macroNameEnd = -1;
                bool closingParenthesisFound = false;
                for (; macroInvokationEnd <= max_i; ++macroInvokationEnd) {
                    const QChar ch = str.at(macroInvokationEnd);
                    if (ch == QLatin1Char(':')) {
                        if (macroNameEnd < 0)
                            macroNameEnd = macroInvokationEnd;
                    } else if (ch == QLatin1Char(')')) {
                        closingParenthesisFound = true;
                        break;
                    }
                }
                if (!closingParenthesisFound)
                    throw Exception(QLatin1String("Macro invocation $( without closing ) found"));

                if (macroNameEnd < 0) {
                    // found standard macro invocation a la $(MAKE)
                    macroNameEnd = macroInvokationEnd;
                }

                const QString macroName = str.mid(i + 1, macroNameEnd - i - 1);
                if (macroName.isEmpty())
                    throw Exception(QLatin1String("Macro name is missing from invocation"));

                switch (macroName.at(0).toLatin1())
                {
                case '<':
                case '*':
                case '@':
                case '?':
                    {
                        ret.append(fileNameMacroMagicEscape);
                        ret.append(QLatin1String("("));
                        ret.append(str.mid(i + 1, macroInvokationEnd - i));
                    }
                    break;
                default:
                    {
                        QString macroValue = cycleCheckedMacroValue(macroName, usedMacros);
                        macroValue = expandMacros(macroValue, inDependentsLine, usedMacros);
                        if (macroNameEnd != macroInvokationEnd) {
                            const Substitution s = parseSubstitutionStatement(str, macroNameEnd + 1, macroInvokationEnd);
                            applySubstitution(s, macroValue);
                        }
                        usedMacros.remove(macroName);
                        ret.append(macroValue);
                    }
                }
                i = macroInvokationEnd;
            } else if (str.at(i) == QLatin1Char('$')) {
                bool fileNameMacroFound = false;
                if (inDependentsLine) {
                    // in a dependents line detect $$@ and handle as $@
                    int j = i + 1;
                    bool parenthesisFound = false;
                    if (str.count() > j && str.at(j) == QLatin1Char('(')) {
                        parenthesisFound = true;
                        ++j;
                    }
                    if (str.count() > j && str.at(j) == QLatin1Char('@')) {
                        fileNameMacroFound = true;
                        ret.append(fileNameMacroMagicEscape);
                        if (parenthesisFound)
                            ret.append(QLatin1Char('('));
                        ret.append(QLatin1Char('@'));
                        i = j;
                    }
                }
                if (!fileNameMacroFound) {
                    // found escaped $ char
                    ret.append(QLatin1Char('$'));
                }
            } else if (str.at(i).isLetterOrNumber()) {
                // found single character macro invocation a la $X
                const QString macroName = str.at(i);
                QString macroValue = cycleCheckedMacroValue(macroName, usedMacros);
                macroValue = expandMacros(macroValue, inDependentsLine, usedMacros);
                usedMacros.remove(macroName);
                ret.append(macroValue);
            } else {
                switch (str.at(i).toLatin1())
                {
                case '<':
                case '*':
                case '@':
                case '?':
                    ret.append(fileNameMacroMagicEscape);
                    ret.append(str.at(i));
                    break;
                default:
                    throw Exception(QLatin1String("Invalid macro invocation found"));
                }
            }
        } else {
            if (appendStart == -1) {
                appendStart = i;
                appendLength = 1;
            } else {
                ++appendLength;
            }
        }
        ++i;
    }

    if (appendStart != -1) {
        if (appendStart == 0 && appendLength == str.count())
            return str;
        ret.append(str.mid(appendStart, appendLength));
    }

    return ret;
}

QString MacroTable::cycleCheckedMacroValue(const QString& macroName, QSet<QString>& usedMacros) const
{
    if (usedMacros.contains(macroName)) {
        QString msg = QLatin1String("Cycle in macro detected when trying to invoke $(%1).");
        throw Exception(msg.arg(macroName));
    }
    usedMacros.insert(macroName);
    return macroValue(macroName);
}

void MacroTable::dump() const
{
    QHash<QString, MacroData>::const_iterator it = m_macros.begin();
    for (; it != m_macros.end(); ++it) {
        printf("%s = %s\n", qPrintable(it.key()), qPrintable((*it).value));
    }
}

/**
 * Invokes a macro value substitution.
 *
 * str:                    $(DEFINES:foo=bar)
 * substitutionStartIdx:             ^
 * equalsSignIdx:                       ^
 * macroInvokationEndIdx:                   ^
 */
MacroTable::Substitution MacroTable::parseSubstitutionStatement(const QString &str,
                                                                int substitutionStartIdx,
                                                                int &macroInvokationEndIdx)
{
    macroInvokationEndIdx = -1;
    int equalsSignIdx = -1;
    bool quoted = false;
    QVector<int> quotePositions;
    for (int i=substitutionStartIdx; i < str.length(); ++i) {
        const QChar &ch = str.at(i);
        if (ch == QLatin1Char('=')) {
            quoted = false;
            equalsSignIdx = i;
        } else if (ch == QLatin1Char(')') && !quoted) {
            quoted = false;
            macroInvokationEndIdx = i;
            break;
        } else if (ch == QLatin1Char('^')) {
            quoted = true;
            quotePositions.append(i);
        } else {
            quoted = false;
        }
    }

    if (equalsSignIdx < 0 || macroInvokationEndIdx < 0)
        throw Exception(QLatin1String("Cannot find = after : in macro substitution."));

    Substitution result;
    result.before = str.mid(substitutionStartIdx, equalsSignIdx - substitutionStartIdx);
    result.after = str.mid(equalsSignIdx + 1, macroInvokationEndIdx - equalsSignIdx - 1);
    for (int i=quotePositions.count() - 1; i >= 0; --i)
        result.after.remove(quotePositions.at(i) - equalsSignIdx - 1, 1);
    return result;
}

void MacroTable::applySubstitution(const MacroTable::Substitution &substitution, QString &value)
{
    value.replace(substitution.before, substitution.after);
}

} // namespace NMakeFile
