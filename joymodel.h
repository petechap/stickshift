#if !defined(INCLUDED_JOYMODEL_H_)
#define INCLUDED_JOYMODEL_H_

/* (c) Peter Chapman 2010
   Licensed under the GNU General Public Licence; either version 2 or (at your
   option) any later version
*/

#include <linux/joystick.h>
#include <libxml/tree.h>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>
#include <boost/signals2.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <vector>
#include <map>
#include <set>

typedef std::vector<__u16> ButtonMap;
typedef std::vector<__u8> AxisMap;

typedef boost::signals2::signal<void (__u32 time,
                                      __s16 value,
                                      bool init)> ChangeSig;
class InputBase
{
protected:
    ChangeSig m_change;
    
public:
    virtual void Input(__u32 time, __s16 value, bool init) = 0;
    virtual boost::signals2::connection Connect(
            const ChangeSig::slot_function_type &f)
    {
        return m_change.connect(f);
    }
    virtual __s16 GetValue() const = 0;
};

class Axis : public InputBase
{
    __u8      m_mapping;
    __s16     m_value;
    
public:
    virtual void Input(__u32 time, __s16 value, bool init) {
        if (init)
            m_value = value;
        else if (value == m_value)
            return;
        
        m_change(time, value, init);
        m_value = value;
    }
    virtual __u8 GetMapping()  const { return m_mapping; }
    virtual __s16 GetValue()   const { return m_value; }
    
    Axis(__u8 mapping = ABS_MISC)
        : m_mapping(mapping),
          m_value(0)
    {
    }
};
typedef boost::shared_ptr<Axis> AxisPtr;

class Button : public InputBase
{
    __u16     m_mapping;
    unsigned  m_order;
    __s16     m_value;
    bool      m_initialised;
    
public:
    virtual void Input(__u32 time, __s16 value, bool init) {
        if (!m_initialised)
        {
            m_value = value;
            m_initialised = true;
        }
        else if (value == m_value)
            return;
        
        m_change(time, value, init);
        m_value = value;
    }
    
    virtual __u16 GetMapping() const { return m_mapping; }
    virtual __s16 GetValue()   const { return m_value; }
    virtual unsigned GetOrder() const { return m_order; }
    
    Button(__u16 mapping = BTN_MISC, unsigned order = 0)
        : m_mapping(mapping),
          m_order(order),
          m_value(0),
          m_initialised(false)
    {
    }
};
typedef boost::shared_ptr<Button> ButtonPtr;

struct ButtonOrder
{
    bool operator()(const ButtonPtr &a, const ButtonPtr &b) const
    {
        if (!a && !b)
            return false;
        else if (a && b)
            return a->GetOrder() == b->GetOrder() ?
                (a < b) : (a->GetOrder() < b->GetOrder());
        else
            return b;
    }
};

typedef std::map<ButtonPtr, ButtonPtr>      ButtonMapping;
typedef boost::shared_ptr<ButtonMapping>    ButtonMappingPtr;
typedef std::set<ButtonPtr, ButtonOrder>    ButtonSet;
typedef boost::shared_ptr<ButtonSet>        ButtonSetPtr;
typedef std::map<std::string, ButtonSet>    ButtonSetMap;
typedef std::map<unsigned, js_corr>         Calibration;
typedef boost::shared_ptr<Calibration>      CalibrationPtr;

struct InputContext;
class ShiftSet;
typedef boost::shared_ptr<ShiftSet> ShiftSetPtr;
class ShiftSet : public boost::enable_shared_from_this<ShiftSet>
{
    
    typedef std::pair<ButtonPtr, __s16> Condition;
    typedef std::map<Button*, std::vector<ButtonPtr> > ShiftMap;
    typedef std::map<Condition, std::list<unsigned> > RotationMap;
    
    struct ConditionState
    {
        Condition                condition;
        std::vector<ShiftSetPtr> subShifts;
    };
    
    ShiftSet(ButtonSetPtr inputButtons);
    
    void ShiftInput(__u32 time, __s16 value, bool init, __u16 testValue,
                     std::list<unsigned> &rotations);
    
    void Input(__u32 time, __s16 value, bool init, Button *inButton);
    
    ButtonSetPtr                  m_inputButtons;
    unsigned                      m_currentSet;
    ShiftMap                      m_shiftMap;
    RotationMap                   m_rotationMap;
    std::vector<ConditionState>   m_conditionStates;
    
public:
    static boost::shared_ptr<ShiftSet> Create(ButtonSetPtr input);
    
    ButtonMappingPtr AddCondition(ButtonPtr button, __s16 state,
                                  const ButtonMapping &sharedButtons,
                                  unsigned &buttonOrder);
    void SetSubShifts(const std::vector<ShiftSetPtr> &shifts);
    
    void AllOutputs(ButtonSet &outputs) const;
    ButtonSetPtr Inputs() const { return m_inputButtons; }
};

class HatButton : public Button
{
    const bool m_positive; // button is pressed when axis positive or negative?
    HatButton(bool positive) : m_positive(positive) { }
public:
    static boost::shared_ptr<HatButton> Create(AxisPtr axis, bool positive)
    {
        boost::shared_ptr<HatButton> button(new HatButton(positive));
        axis->Connect(ChangeSig::slot_type(&HatButton::Input, button.get(),
                                           _1, _2, _3).track(button));
        return button;
    }
    
    virtual void Input(__u32 time, __s16 value, bool init)
    {
        __s16 pressed = (m_positive ? value : -value) > 0 ? 1 : 0;
        Button::Input(time, pressed, init);
    }
};

class Joystick
{
protected:
    std::string m_name;
    
public:
    virtual std::string GetName() const { return m_name; }
    virtual unsigned    NumAxes() const = 0;
    virtual unsigned    NumButtons() const = 0;
    virtual AxisPtr     GetAxis(unsigned) const = 0;
    virtual ButtonPtr   GetButton(unsigned) const = 0;
    
    virtual void GetCorrection(js_corr *) const = 0;
    virtual void SetCorrection(const js_corr *) = 0;
    
    virtual void Calibrate(CalibrationPtr);
    
    virtual ~Joystick() {};
};
typedef boost::shared_ptr<Joystick> JoystickPtr;

typedef boost::function<ButtonSetPtr (const std::string &)> LookupFn;

struct InputContext
{
    std::vector<AxisPtr> axes;
    ButtonSetMap         buttons;
    unsigned             buttonOrder;
    ButtonSet            conditionals;
    std::vector<ButtonMappingPtr> layers;
    
    InputContext() : buttonOrder(0) {}
};

class MappedJoystick : public Joystick
{
    std::vector<ButtonPtr> m_buttons;
    std::vector<unsigned>  m_axes; // indices into axes in m_in
    JoystickPtr            m_in;
    const char            *m_configOut;
    
    std::vector<ShiftSetPtr>  m_shifts;
    boost::shared_ptr<xmlDoc> m_xmlDoc;
    
public: 
    virtual unsigned    NumAxes() const { return m_axes.size(); }
    virtual unsigned    NumButtons() const { return m_buttons.size(); }
    virtual ButtonPtr   GetButton(unsigned i) const { return m_buttons[i]; }
    
    virtual void GetCorrection(js_corr *) const;
    virtual void SetCorrection(const js_corr *);
    virtual AxisPtr GetAxis(unsigned i) const;
    
    MappedJoystick(JoystickPtr in, const char *mapfile, const char *corrfile);
};

class InputJoystick : public Joystick
{
    std::vector<ButtonPtr> m_buttons;
    std::vector<AxisPtr>   m_axes;
    int                    m_fd;
    
public:
    InputJoystick(int fd);
    
    virtual std::string GetName() const { return m_name; }
    virtual unsigned    NumAxes() const { return m_axes.size(); }
    virtual unsigned    NumButtons() const { return m_buttons.size(); }
    virtual AxisPtr     GetAxis(unsigned i) const { return m_axes[i]; }
    virtual ButtonPtr   GetButton(unsigned i) const { return m_buttons[i]; }
    virtual void GetCorrection(js_corr *) const;
    virtual void SetCorrection(const js_corr *);
};

#endif
