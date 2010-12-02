/* (c) Peter Chapman 2010
   Licensed under the GNU General Public Licence; either version 2 or (at your
   option) any later version
*/
#include <sys/ioctl.h>
#include "joymodel.h"

#include <iostream>
#include <libxml/parser.h>
#include <boost/lexical_cast.hpp>
#include <boost/scoped_array.hpp>
#include <boost/tokenizer.hpp>
#include <boost/format.hpp>
#include <boost/foreach.hpp>

#define SS_JS_CORR_UNMAPPED 0x10

ShiftSet::ShiftSet(ButtonSetPtr inputButtons)
 : m_inputButtons(inputButtons),
   m_currentSet(0)
{
}

ShiftSetPtr ShiftSet::Create(ButtonSetPtr input)
{
    using namespace boost;
    ShiftSetPtr p(new ShiftSet(input));
    BOOST_FOREACH(ButtonPtr i, *input)
        i->Connect(ChangeSig::slot_type(&ShiftSet::Input, p.get(),
                                        _1, _2, _3, i.get()).track(p).track(i));
    return p;
}

ButtonMappingPtr ShiftSet::AddCondition(
        ButtonPtr button, __s16 state,
        const ButtonMapping &sharedButtons,
        unsigned &buttonOrder)
{
    using namespace boost;
    
    ButtonMappingPtr outputs(new ButtonMapping());
    
    // Shift buttons don't appear in the output
    assert(m_inputButtons->find(button) == m_inputButtons->end());
    
    const bool firstSet = m_shiftMap.empty();
    for (ButtonSet::iterator i = m_inputButtons->begin();
         i != m_inputButtons->end(); ++i)
    {
        __u16 mapping = (*i)->GetMapping();
        unsigned order = firstSet ? (*i)->GetOrder() : buttonOrder++;
        ButtonMapping::const_iterator reuse = sharedButtons.find(*i);
        ButtonPtr newButton =
            reuse==sharedButtons.end() ? make_shared<Button>(mapping, order)
                                       : reuse->second;
        std::vector<ButtonPtr> &sets = m_shiftMap[i->get()];
        assert(sets.size() == m_conditionStates.size());
        
        sets.push_back(newButton);
        
        (*outputs)[*i] = newButton;
    }
    
    unsigned shiftIndex = m_conditionStates.size();
    Condition condition(button, state);
    if (m_rotationMap[condition].empty())
    {
        button->Connect(ChangeSig::slot_type(
            &ShiftSet::ShiftInput, this, _1, _2, _3, state,
            boost::ref(m_rotationMap[condition])).track(shared_from_this()));
    }
    m_rotationMap[condition].push_back(shiftIndex);
    
    ConditionState cs = { condition };
    m_conditionStates.push_back(cs);
    
    return outputs;
}

void ShiftSet::SetSubShifts(const std::vector<ShiftSetPtr> &shifts)
{
    m_conditionStates.back().subShifts = shifts;
}

void ShiftSet::AllOutputs(ButtonSet &outputs) const
{
    // remove our inputs
    BOOST_FOREACH (ButtonPtr b, *m_inputButtons)
        outputs.erase(b);
    
    // Add our outputs
    BOOST_FOREACH (const ShiftMap::value_type &v, m_shiftMap)
        outputs.insert(v.second.begin(), v.second.end());
    
    // Add the output of subshifts
    BOOST_FOREACH (const ConditionState &c, m_conditionStates)
    {
        // Remove condition button
        outputs.erase(c.condition.first);
        
        BOOST_FOREACH (const ShiftSetPtr &ss, c.subShifts)
            ss->AllOutputs(outputs);
    }
}

void ShiftSet::Input(__u32 time, __s16 value, bool init, Button *inButton)
{
    ShiftMap::iterator i = m_shiftMap.find(inButton);
    if (i == m_shiftMap.end())
        return;
    std::vector<ButtonPtr> &outputs = i->second;
    
    if (m_currentSet >= outputs.size())
        return;
    
    outputs[m_currentSet]->Input(time, value, init);
    
    if (init)
        for (unsigned j = 0; j < outputs.size(); ++j)
            if (j != m_currentSet)
                outputs[j]->Input(time, 0, init);
}

void ShiftSet::ShiftInput(__u32 time, __s16 value, bool init, __u16 testValue,
                          std::list<unsigned> &rotations)
{
    if (value != testValue)
        return;

    // move front index to the back
    rotations.splice(rotations.end(), rotations, rotations.begin());
    
    unsigned newSet = rotations.front();
    if (m_currentSet == newSet)
        return; // already selected - nothing to do
    
    // Copy values of previously selected buttons to the newly selected
    // buttons, and set the former to 0
    for (ShiftMap::iterator i = m_shiftMap.begin(); i != m_shiftMap.end(); ++i)
    {
        using namespace std;
        std::vector<ButtonPtr> &shifts = i->second;
        if (shifts[newSet] == shifts[m_currentSet])
            continue;
        
        __s16 oldval = shifts[m_currentSet]->GetValue();
        shifts[m_currentSet]->Input(time, 0, init);
        shifts[newSet]->Input(time, oldval, init);
    }
    m_currentSet = newSet;
}

bool GetProp(xmlNode *node, const char *name, std::string &val)
{
   xmlChar *xmlVal = xmlGetProp(node, BAD_CAST name);
   if (!xmlVal)
       return false;
   
   val = (const char*)xmlVal;
   xmlFree(xmlVal);
   return true;
}

ButtonSetPtr ParseAxisButtons(xmlNode *node, InputContext &context)
{
    using namespace boost;
    std::string axisStr, name;
    ButtonSetPtr retVal;
    if (strcmp((const char*)node->name, "axisbuttons") != 0)
        return retVal;
    
    if (!GetProp(node, "axis", axisStr))
        return retVal;
    unsigned axis;
    try {
        axis = lexical_cast<unsigned>(axisStr);
    } catch (boost::bad_lexical_cast &) {
        axis = context.axes.size(); // invalid
    }
    if (axis >= context.axes.size() || !context.axes[axis])
        throw std::runtime_error(
            str(format("no such axis '%s'") % axisStr));

    AxisPtr &axisPtr = context.axes[axis];
    ButtonPtr neg = HatButton::Create(axisPtr, false);
    ButtonPtr pos = HatButton::Create(axisPtr, true);
    
    retVal = make_shared<ButtonSet>();
    retVal->insert(neg);
    retVal->insert(pos);
    
    context.buttons[""].insert(retVal->begin(), retVal->end());
    
    if (GetProp(node, "neg_name", name))
    {
        context.buttons[name].clear();
        context.buttons[name].insert(neg);
    }
    if (GetProp(node, "pos_name", name))
    {
        context.buttons[name].clear();
        context.buttons[name].insert(pos);
    }

    axisPtr.reset(); // remove from output axes
    
    return retVal;
}

ButtonSetPtr Lookup(InputContext &context, const std::string &name)
{
    ButtonSetMap::const_iterator i = context.buttons.find(name);
    if (i == context.buttons.end() || i->second.empty())
        return ButtonSetPtr();
    ButtonSetPtr ret(new ButtonSet(i->second));
    
    BOOST_FOREACH (ButtonMappingPtr &bm, context.layers)
    {
        BOOST_FOREACH (ButtonMapping::value_type &i, *bm)
        {
            ButtonSet::iterator j = ret->find(i.first);
            if (j != ret->end())
            {
                ret->erase(j);
                if (i.second)
                    ret->insert(i.second);
            }
        }
    }
    
    if (ret->empty())
        return ButtonSetPtr();

    return ret;
}

ButtonSetPtr LookupMultiple(InputContext &context, const std::string &names)
{
    using namespace boost;
    typedef tokenizer<char_separator<char> > Tokenizer;
    
    ButtonSetPtr bset(new ButtonSet());
    char_separator<char> sep(",; ");
    Tokenizer tok(names, sep);
    for (Tokenizer::iterator i = tok.begin(); i != tok.end(); ++i)
    {
        if (ButtonSetPtr bs = Lookup(context, *i))
            bset->insert(bs->begin(), bs->end());
        else
            throw std::runtime_error(
                str(format("Can't find use name '%s'") % *i));
    }
    return bset;
}


ButtonSetPtr ParseBset(xmlNode *bsetNode, InputContext &context,
                       bool overwrite = true, std::string *name = 0)
{
    using namespace boost;
    if (strcmp((const char*)bsetNode->name, "bset") != 0)
        return ButtonSetPtr();
        
    ButtonSetPtr bset;
    ButtonSetMap::iterator ctxi;
    std::string beginStr, endStr;
    
    std::string val;
    if (GetProp(bsetNode, "use", val))
        bset = LookupMultiple(context, val);
    else
        bset.reset(new ButtonSet());
    
    if (GetProp(bsetNode, "begin", beginStr) &&
        GetProp(bsetNode, "end", endStr))
    {
        unsigned begin = lexical_cast<unsigned>(beginStr);
        unsigned end = lexical_cast<unsigned>(endStr);
        for (; begin <= end; ++begin)
        {
            std::string idxStr = lexical_cast<std::string>(begin);
            if (ButtonSetPtr bs = Lookup(context, idxStr))
                bset->insert(bs->begin(), bs->end());
        }
    }
    for (xmlNode *i = bsetNode->children; i; i = i->next)
    {
        if (i->type != XML_ELEMENT_NODE)
            continue;
        if (ButtonSetPtr toAdd = ParseBset(i, context))
            bset->insert(toAdd->begin(), toAdd->end());
        if (ButtonSetPtr toAdd = ParseAxisButtons(i, context))
            bset->insert(toAdd->begin(), toAdd->end());
    }
    
    if (GetProp(bsetNode, "name", val))
    {
        if (name)
            *name = val;
        
        if (overwrite)
            context.buttons[val] = *bset;
    }
    else if (name)
        *name = "";

    return bset;
}

bool ParseReuse(xmlNode *node, InputContext &context,
                ButtonMapping &shared, ButtonSetPtr inputs)
{
    using namespace boost;
    if (node->type != XML_ELEMENT_NODE ||
        (strcmp((const char*)node->name, "reuse") != 0))
    {
        return false;
    }
    
    std::string toReplace, replaceWith;
    if (!GetProp(node, "replace", toReplace) ||
        !GetProp(node, "with", replaceWith))
    {
        return false;
    }
    
    ButtonSetPtr mapInput = LookupMultiple(context, toReplace);
    ButtonSetPtr mapOutput = LookupMultiple(context, replaceWith);
    
    if (mapInput->size() != mapOutput->size())
        throw std::runtime_error(str(format(
            "'%s' and '%s' are of different size") % toReplace % replaceWith));
    
    ButtonSet::iterator i, o;
    for (i = mapInput->begin(), o = mapOutput->begin();
         i != mapInput->end(); ++i, ++o)
    {
        if (inputs->find(*i) == inputs->end())
        {
            throw std::runtime_error(
                "condition bset contains button from outside shift");
        }
        if (*i == *o)
            throw std::runtime_error(
                "condition bset is circular");
        
        shared[*i] = *o;
    }
}

const char *bLineCoefNames[4] = { "centre_min", "centre_max",
                                  "slope_neg", "slope_pos" };
const int bLineCoefs = sizeof(bLineCoefNames)/sizeof(*bLineCoefNames);

CalibrationPtr ParseCalibrate(xmlNode *calNode)
{
    using namespace boost;
    CalibrationPtr cal;
    if (strcmp((const char*)calNode->name, "calibrate") != 0)
        return cal;
    
    cal.reset(new Calibration());
    for (xmlNode *i = calNode->children; i; i = i->next)
    {
        js_corr entry = js_corr();
        
        if (i->type != XML_ELEMENT_NODE)
            continue;
        else if (strcmp((const char*)i->name, "broken_line") == 0)
            entry.type = JS_CORR_BROKEN;
        else if (strcmp((const char*)i->name, "none") == 0)
            entry.type = JS_CORR_NONE;
        else
            continue;
        
        std::string axisStr;
        if (!GetProp(i, "axis", axisStr))
            continue;
        unsigned axis = lexical_cast<unsigned>(axisStr);
        
        std::string precStr;
        if (GetProp(i, "precision", precStr))
            entry.prec = lexical_cast<__s16>(precStr);
        
        if (entry.type == JS_CORR_BROKEN)
        {
            for (unsigned j = 0; j < bLineCoefs ; ++j)
            {
                std::string val;
                if (!GetProp(i, bLineCoefNames[j], val))
                    throw std::runtime_error(str(format(
                        "broken_line calibration element must contain '%s'")
                                % bLineCoefNames[j]));
                entry.coef[j] = lexical_cast<__s32>(val);
            }
        }
        
        (*cal)[axis] = entry;
    }
    return cal;
}

ShiftSetPtr ParseShift(xmlNode *shiftNode, InputContext &context);

ButtonPtr ParseCondition(xmlNode *condNode, InputContext &context,
                         ShiftSetPtr shift = ShiftSetPtr())
{
    using namespace boost;
    typedef tokenizer<char_separator<char> > Tokenizer;
    
    ButtonPtr button;
    
    if (strcmp((const char*)condNode->name, "condition") != 0)
        return button;
    
    std::string val;
    if (!GetProp(condNode, "button", val))
        return button;
    
    ButtonSetPtr i = Lookup(context, val);
    if (!i)
        throw std::runtime_error(str(format("button '%s' not found") % val));
    
    const ButtonSet &bset = *i;
    if (bset.size() != 1)
        throw std::runtime_error(
            "'button' attribute of condition element XML element must refer "
            "to a single button");
        
    button = *bset.begin();
    if (!shift)
        return button;
    
    std::string statesStr = "1";
    GetProp(condNode, "state", statesStr);
    
    char_separator<char> sep(",; ");
    Tokenizer tok(statesStr, sep);
    std::vector<std::string> states(tok.begin(), tok.end());
    
    if (states.size() > 1 && GetProp(condNode, "name", val))
        throw std::runtime_error(
           "Condition name not valid for multiple conditions");
    if (states.empty())
        throw std::runtime_error(
                str(format("Bad button state '%s'") % statesStr));

    const bool parseChildren = states.size() == 1;
    
    ButtonMapping sharedButtons;
    if (parseChildren)
        for (xmlNode *i = condNode->children; i; i = i->next)
            ParseReuse(i, context, sharedButtons, shift->Inputs());
    
    ButtonMappingPtr newButtons;
    std::vector<ShiftSetPtr> subShifts;
    for (unsigned i = 0; i < states.size(); ++i)
    {
        newButtons = shift->AddCondition(button, lexical_cast<__u16>(states[i]),
                                         sharedButtons, context.buttonOrder);
        if (!parseChildren)
            shift->SetSubShifts(subShifts);
        
        if (GetProp(condNode, "name", val))
        {
            context.buttons[val].clear();
            BOOST_FOREACH (ButtonMapping::value_type &v, *newButtons)
                context.buttons[val].insert(v.second);
        }
    }
    
    if (parseChildren)
    {
        context.layers.push_back(newButtons);
        
        for (xmlNode *i = condNode->children; i; i = i->next)
        {
            if (ShiftSetPtr ss = ParseShift(i, context))
                subShifts.push_back(ss);
            else
                ParseBset(i, context);
        }
        
        shift->SetSubShifts(subShifts);
        
        context.layers.pop_back();
    }
    
    return button;
}

void Erase(InputContext &context, const ButtonSet &bs)
{
    ButtonSet toDel(bs);
    unsigned before = toDel.size();
    
    BOOST_REVERSE_FOREACH(ButtonMappingPtr &bm, context.layers)
    {
        BOOST_FOREACH(ButtonMapping::value_type &v, *bm)
        {
            ButtonSet::iterator i = toDel.find(v.second);
            if (i != toDel.end())
            {
                v.second.reset();
                toDel.erase(i);
            }
        }
    }
    
    std::cerr << "erased " << before - toDel.size() << " from layers and ";
    before = toDel.size();
    
    ButtonSet deleted;
    for (ButtonSetMap::iterator i = context.buttons.begin();
         i != context.buttons.end();)
    {
        BOOST_FOREACH (const ButtonPtr &b, toDel)
            if (i->second.erase(b))
                deleted.insert(b);
        
        if (i->second.empty())
            context.buttons.erase(i++);
        else
            ++i;
    }
    
    std::cerr << before - toDel.size() << " from base map\n";
    assert(deleted == toDel);
}

ShiftSetPtr ParseShift(xmlNode *shiftNode, InputContext &context)
{
    using namespace boost;
    ShiftSetPtr shift;
    if (strcmp((const char*)shiftNode->name, "shift") != 0)
        return shift;
    
    ButtonSetPtr inputSet;
    ButtonSet conditionButtons;
    // Iterate element once to get all input button sets
    for (xmlNode *i = shiftNode->children; i; i = i->next)
    {
        if (i->type != XML_ELEMENT_NODE)
            continue;
        if (ButtonSetPtr newSet = ParseBset(i, context))
        {
            if (!inputSet)
                inputSet = make_shared<ButtonSet>();
            inputSet->insert(newSet->begin(), newSet->end());
        }
        else if (ButtonPtr condBtn = ParseCondition(i, context))
        {
            // also build a list of buttons involved in switching
            conditionButtons.insert(condBtn);
        }
    }
    
    if (!inputSet)
    {
        // No bset node given - use all available buttons
        if (context.layers.empty())
        {
            std::cerr << "No bset given: using "<<context.buttons[""].size()<<" top-level buttons\n";
            inputSet.reset(new ButtonSet());
            ButtonSet &all = context.buttons[""];
            std::set_difference(all.begin(), all.end(),
                                context.conditionals.begin(), context.conditionals.end(),
                                inserter(*inputSet, inputSet->end()), ButtonOrder());
        }
        else
        {
            inputSet.reset(new ButtonSet());
            
            BOOST_FOREACH (ButtonMapping::value_type &b, *context.layers.back())
                if (b.second)
                    inputSet->insert(b.second);
            std::cerr << "No bset given: using "<<inputSet->size()<<" inherited buttons\n";
        }
    }
    
    context.conditionals.insert(conditionButtons.begin(),
                                conditionButtons.end());
    // condition buttons are not inputs to the ShiftSet and do not appear in
    // the output
    BOOST_FOREACH(const ButtonPtr &b, context.conditionals)
        inputSet->erase(b);
    std::cerr << "After removing conditions: "<<inputSet->size()<<" buttons\n";
        
    shift = ShiftSet::Create(inputSet);
    
    // Iterate element again, picking out conditions this time
    for (xmlNode *i = shiftNode->children; i; i = i->next)
        if (i->type == XML_ELEMENT_NODE)
            ParseCondition(i, context, shift);
    
    std::string name;
    if (GetProp(shiftNode, "name", name))
    {
        context.buttons[name].clear();
        shift->AllOutputs(context.buttons[name]);
    }
    
    Erase(context, *inputSet);
    
    return shift;
}

MappedJoystick::MappedJoystick(JoystickPtr in, const char *mapfile,
                               const char *configOut)
    : m_in(in),
      m_configOut(configOut)
{
    using namespace boost;
    m_name = std::string("StickShift: ") + in->GetName();

    InputContext input;
    
    for (unsigned i = 0; i < in->NumAxes(); ++i)
        input.axes.push_back(in->GetAxis(i));

    for (unsigned i = 0; i < in->NumButtons(); ++i)
    {
        ButtonPtr inButton = in->GetButton(i);
        ButtonSet bset;
        bset.insert(inButton);
        input.buttonOrder = std::max(input.buttonOrder, inButton->GetOrder());
        input.buttons[lexical_cast<std::string>(i)] = bset;
        input.buttons[""].insert(inButton);
    }
    
    xmlDoc *doc = 0;

    xmlLineNumbersDefault(1);
    if (xmlDoc *doc = xmlReadFile(mapfile, NULL, XML_PARSE_NOERROR))
    {
        m_xmlDoc.reset(doc, bind(&xmlFreeDoc, _1));
    }
    else
    {
        xmlErrorPtr err = xmlGetLastError();
        std::string msg;
        if (err)
            msg = str(format("Error reading %s, line %d: %s") % mapfile
                                                              % err->line
                                                              % err->message);
        else
            msg = str(format("Error reading %s") % mapfile);
        
        throw std::runtime_error(msg);
    }
    
    xmlNode *root = xmlDocGetRootElement(m_xmlDoc.get());
    for (xmlNode *i = root->children; i; i = i->next)
    {
        if (i->type != XML_ELEMENT_NODE)
            continue;
        else if (ParseBset(i, input) || ParseAxisButtons(i, input))
            continue;
        else if (ShiftSetPtr p = ParseShift(i, input)) 
            m_shifts.push_back(p);
        else if (CalibrationPtr cal = ParseCalibrate(i))
            m_in->Calibrate(cal);
    }
    
    ButtonSet all = input.buttons[""];
    BOOST_FOREACH (ShiftSetPtr ss, m_shifts)
        ss->AllOutputs(all);
    
    std::set_difference(all.begin(), all.end(),
                        input.conditionals.begin(), input.conditionals.end(),
                        back_inserter(m_buttons), ButtonOrder());
    
    for (unsigned inIdx = 0; inIdx < input.axes.size(); ++inIdx)
        if (input.axes[inIdx])
            m_axes.push_back(inIdx);
}

void MappedJoystick::GetCorrection(js_corr *out) const
{
    
    boost::scoped_array<js_corr> orig(new js_corr[m_in->NumAxes()]());
    m_in->GetCorrection(orig.get());
    for (unsigned i = 0; i < m_axes.size(); ++i)
        out[i] = orig[m_axes[i]];
}

AxisPtr MappedJoystick::GetAxis(unsigned i) const
{
    return m_in->GetAxis(m_axes[i]);
}

void RemoveAutogeneratedCalibrations(xmlNode *root)
{
    for (xmlNode *i = root->children; i;)
    {
        std::string autogen;
        if (i->type != XML_ELEMENT_NODE ||
            !ParseCalibrate(i) ||
            !GetProp(i, "autogenerated", autogen) ||
            autogen != "true")
        {
            i = i->next;
            continue;
        }
        
        xmlNode *next = i->next, *thisNode = i;
        // remove single trailing newline
        if (next && xmlNodeIsText(next) && next->content &&
            strcmp((const char*)i->next->content, "\n") == 0)
        {
            xmlUnlinkNode(next);
            xmlFreeNode(next);
        }
        
        i = i->next;
        xmlUnlinkNode(thisNode);
        xmlFreeNode(thisNode);
    }
}

void AddCalibrationElement(xmlNode *root, const js_corr *cal, unsigned num)
{
    using namespace boost;
    using std::string;
    bool addNewline = true;
    if (root->last && xmlNodeIsText(root->last))
        if (xmlChar *content = root->last->content)
            if (int len = xmlStrlen(content))
                    addNewline = content[len-1] != '\n';
    if (addNewline)
        xmlAddChild(root, xmlNewText(BAD_CAST "\n"));
    
    xmlNode *calNode = xmlNewChild(root, NULL, BAD_CAST "calibrate",
                                   BAD_CAST "\n  ");
    xmlNewProp(calNode, BAD_CAST "autogenerated", BAD_CAST "true");
    for (int i = 0; i < num; ++i)
    {
        if (cal[i].type == SS_JS_CORR_UNMAPPED)
        {
            string msg = str(format(" axis %s is mapped to hat buttons ") % i);
            xmlAddChild(calNode, xmlNewComment(BAD_CAST msg.c_str()));
        }
        else
        {
            const char *type = (cal[i].type == JS_CORR_BROKEN) ? "broken_line"
                                                               : "none";
            xmlNode *axisNode = xmlNewChild(calNode, NULL, BAD_CAST type, NULL);
            xmlNewProp(axisNode, BAD_CAST "axis",
                       BAD_CAST lexical_cast<string>(i).c_str());
            xmlNewProp(axisNode, BAD_CAST "precision",
                       BAD_CAST lexical_cast<string>(cal[i].prec).c_str());
            if (cal[i].type == JS_CORR_BROKEN)
            {
                const __s32 *coef = cal[i].coef;
                for (unsigned j = 0; j < bLineCoefs ; ++j)
                    xmlNewProp(axisNode, BAD_CAST bLineCoefNames[j],
                               BAD_CAST lexical_cast<string>(coef[j]).c_str());
            }
        }
        
        bool final = i == num-1;
        xmlAddChild(calNode, xmlNewText(BAD_CAST (final ? "\n" : "\n  ")));
    }
    xmlAddChild(root, xmlNewText(BAD_CAST "\n"));
}

void MappedJoystick::SetCorrection(const js_corr *in)
{
    boost::scoped_array<js_corr> orig(new js_corr[m_in->NumAxes()]);
    
    // Get corrections before we set them so that we can leave any unused axes
    // (ie hat axes that are mapped to buttons) untouched
    m_in->GetCorrection(orig.get());
    for (unsigned i = 0; i < m_axes.size(); ++i)
        orig[m_axes[i]] = in[i];
    m_in->SetCorrection(orig.get());
    
    if (m_configOut)
    {
        const unsigned realAxes = m_in->NumAxes();
        xmlNode *root = xmlDocGetRootElement(m_xmlDoc.get());
        
        // Mark unmapped axes so that we can avoid writing out their
        // (unchanged) correction values to the output file.
        for (unsigned i = 0; i < realAxes; ++i)
            if (std::find(m_axes.begin(), m_axes.end(), i) == m_axes.end())
                orig[i].type = SS_JS_CORR_UNMAPPED;
        
        RemoveAutogeneratedCalibrations(root);
        AddCalibrationElement(root, orig.get(), realAxes);
        xmlSaveFile(m_configOut, m_xmlDoc.get());
    }
}

InputJoystick::InputJoystick(int fd)
    : m_fd(fd)
{
    using namespace boost;
    char namebuf[256];
    memset(namebuf, 0, sizeof(namebuf));
    if (int namelen = ioctl(fd, JSIOCGNAME(sizeof(namebuf)), namebuf))
        m_name = namebuf;
    
    __u16 buttonMap[_IOC_SIZE(JSIOCGBTNMAP)/sizeof(__u16)];
    __u8  axisMap[_IOC_SIZE(JSIOCGAXMAP)/sizeof(__u8)];
    ioctl(fd, JSIOCGBTNMAP,  buttonMap);
    ioctl(fd, JSIOCGAXMAP,   axisMap);
    
    __u8 buttons = 0, axes = 0;
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    ioctl(fd, JSIOCGAXES,    &axes);
    
    for (unsigned i = 0; i < buttons; ++i)
        m_buttons.push_back(make_shared<Button>(buttonMap[i], i));
    for (unsigned i = 0; i < axes; ++i)
        m_axes.push_back(make_shared<Axis>(axisMap[i]));
}

void InputJoystick::GetCorrection(js_corr *corr) const
{
    ioctl(m_fd, JSIOCGCORR, corr);
}

void InputJoystick::SetCorrection(const js_corr *corr)
{
    ioctl(m_fd, JSIOCSCORR, corr);
}

void Joystick::Calibrate(CalibrationPtr cal)
{
    boost::scoped_array<js_corr> corr(new js_corr[NumAxes()]());
    GetCorrection(corr.get());
    BOOST_FOREACH (Calibration::value_type &c, *cal)
    {
        if (c.first >= NumAxes())
            continue;
        corr[c.first] = c.second;
    }
    SetCorrection(corr.get());
}
