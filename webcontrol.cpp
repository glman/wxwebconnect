///////////////////////////////////////////////////////////////////////////////
// Name:        webcontrol.cpp
// Purpose:     wxwebconnect: embedded web browser control library
// Author:      Benjamin I. Williams
// Modified by:
// Created:     2006-09-22
// RCS-ID:      
// Copyright:   (C) Copyright 2006-2010, Kirix Corporation, All Rights Reserved.
// Licence:     wxWindows Library Licence, Version 3.1
///////////////////////////////////////////////////////////////////////////////


#define _CRT_SECURE_NO_WARNINGS


#include <string>
#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/file.h>
#include <wx/event.h>
#include "webframe.h"
#include "webcontrol.h"
#include "nsinclude.h"
#include "domprivate.h"
#include "promptservice.h"
//#include <nsIScriptGlobalObject.h>


// global preference for whether or not to show certificate errors
bool g_ignore_ssl_cert_errors = false;



///////////////////////////////////////////////////////////////////////////////
//  event declarations
///////////////////////////////////////////////////////////////////////////////


DEFINE_EVENT_TYPE(wxEVT_WEB_OPENURI)
DEFINE_EVENT_TYPE(wxEVT_WEB_TITLECHANGE)
DEFINE_EVENT_TYPE(wxEVT_WEB_LOCATIONCHANGE)
DEFINE_EVENT_TYPE(wxEVT_WEB_DOMCONTENTLOADED)
DEFINE_EVENT_TYPE(wxEVT_WEB_STATUSTEXT)
DEFINE_EVENT_TYPE(wxEVT_WEB_STATUSCHANGE)
DEFINE_EVENT_TYPE(wxEVT_WEB_STATECHANGE)
DEFINE_EVENT_TYPE(wxEVT_WEB_SHOWCONTEXTMENU)
DEFINE_EVENT_TYPE(wxEVT_WEB_CREATEBROWSER)
DEFINE_EVENT_TYPE(wxEVT_WEB_LEFTDOWN)
DEFINE_EVENT_TYPE(wxEVT_WEB_MIDDLEDOWN)
DEFINE_EVENT_TYPE(wxEVT_WEB_RIGHTDOWN)
DEFINE_EVENT_TYPE(wxEVT_WEB_LEFTUP)
DEFINE_EVENT_TYPE(wxEVT_WEB_MIDDLEUP)
DEFINE_EVENT_TYPE(wxEVT_WEB_RIGHTUP)
DEFINE_EVENT_TYPE(wxEVT_WEB_LEFTDCLICK)
DEFINE_EVENT_TYPE(wxEVT_WEB_MOUSEOVER)
DEFINE_EVENT_TYPE(wxEVT_WEB_MOUSEOUT)
DEFINE_EVENT_TYPE(wxEVT_WEB_DRAGDROP)
DEFINE_EVENT_TYPE(wxEVT_WEB_INITDOWNLOAD)
DEFINE_EVENT_TYPE(wxEVT_WEB_SHOULDHANDLECONTENT)
DEFINE_EVENT_TYPE(wxEVT_WEB_FAVICONAVAILABLE)
DEFINE_EVENT_TYPE(wxEVT_WEB_DOMEVENT)
IMPLEMENT_DYNAMIC_CLASS(wxWebEvent, wxNotifyEvent)




// the purpose of the EmbeddingPtrs structure is to allow us a way
// to access the ns interfaces without including them in any public
// header file.

struct EmbeddingPtrs
{
    ns_smartptr<nsIWebBrowser> m_web_browser;
    ns_smartptr<nsIWebBrowserFind> m_web_browser_find;
    ns_smartptr<nsIBaseWindow> m_base_window;
    ns_smartptr<nsIWebNavigation> m_web_navigation;
    ns_smartptr<nsIDOMEventTarget> m_event_target;
    ns_smartptr<nsIClipboardCommands> m_clipboard_commands;
    
    ns_smartptr<nsISupports> m_print_settings;
};


// declare a wxArray for ContentListener

class ContentListener;
WX_DEFINE_ARRAY_PTR(ContentListener*, ContentListenerPtrArray);


class PluginListProvider;




class nsIScriptContext;
class nsIArray;
class nsScriptErrorEvent;
class nsEventStatus;
class nsIScriptGlobalObjectOwner;
class nsPresContext;
class nsEvent;
class nsIDocShell;
class nsIDOMWindowInternal;

class nsIScriptContext;
class nsIDOMDocument;
class nsIDOMEvent;
class nsPresContext;
class nsIScriptGlobalObjectOwner;
class nsIArray;
class nsScriptErrorEvent;
class nsIScriptGlobalObject;
struct JSObject; // until we finally remove GetGlobalJSObject...

// Some helpers for working with integer "script type IDs", and specifically
// for working with arrays of such objects. For example, it is common for
// implementations supporting multiple script languages to keep each
// language's nsIScriptContext in an array indexed by the language ID.

// Implementation note: We always ignore nsIProgrammingLanguage::UNKNOWN and
// nsIProgrammingLanguage::CPLUSPLUS - this gives javascript slot 0.  An
// attempted micro-optimization tried to avoid us going all the way to 
// nsIProgrammingLanguage::MAX; however:
// * Someone is reportedly working on a PHP impl - that has value 9
// * nsGenericElement therefore allows 4 bits for the value.
// So there is no good reason for us to be more restrictive again...

#define NS_STID_FIRST nsIProgrammingLanguage::JAVASCRIPT
// like nsGenericElement, only 4 bits worth is valid...
#define NS_STID_LAST (nsIProgrammingLanguage::MAX > 0x000FU ? \
                      0x000FU : nsIProgrammingLanguage::MAX)

// Use to declare the array size
#define NS_STID_ARRAY_UBOUND (NS_STID_LAST-NS_STID_FIRST+1)

// Is a language ID valid?
#define NS_STID_VALID(langID) (langID >= NS_STID_FIRST && langID <= NS_STID_LAST)

// Return an index for a given ID.
#define NS_STID_INDEX(langID) (langID-NS_STID_FIRST)

// Create a 'for' loop iterating over all possible language IDs (*not* indexes)
#define NS_STID_FOR_ID(varName) \
          for (varName=NS_STID_FIRST;varName<=NS_STID_LAST;varName++)

// Create a 'for' loop iterating over all indexes (when you don't need to know
// what language it is)
#define NS_STID_FOR_INDEX(varName) \
          for (varName=0;varName<=NS_STID_INDEX(NS_STID_LAST);varName++)

// A helper function for nsIScriptGlobalObject implementations to use
// when handling a script error.  Generally called by the global when a context
// notifies it of an error via nsIScriptGlobalObject::HandleScriptError.
// Returns PR_TRUE if HandleDOMEvent was actually called, in which case
// aStatus will be filled in with the status.
PRBool
NS_HandleScriptError(nsIScriptGlobalObject *aScriptGlobal,
                     nsScriptErrorEvent *aErrorEvent,
                     nsEventStatus *aStatus);


#define NS_ISCRIPTGLOBALOBJECT_IID \
{ 0xe9f3f2c1, 0x2d94, 0x4722, \
  { 0xbb, 0xd4, 0x2b, 0xf6, 0xfd, 0xf4, 0x2f, 0x48 } }

/**
  * The global object which keeps a script context for each supported script
  * language. This often used to store per-window global state.
 */

class nsIScriptGlobalObject : public nsISupports
{
public:
NS_DEFINE_STATIC_IID_ACCESSOR(NS_ISCRIPTGLOBALOBJECT_IID)
//  NS_DECLARE_STATIC_IID_ACCESSOR(NS_ISCRIPTGLOBALOBJECT_IID)

  /**
   * Ensure that the script global object is initialized for working with the
   * specified script language ID.  This will set up the nsIScriptContext
   * and 'script global' for that language, allowing these to be fetched
   * and manipulated.
   * @return NS_OK if successful; error conditions include that the language
   * has not been registered, as well as 'normal' errors, such as
   * out-of-memory
   */
  virtual nsresult EnsureScriptEnvironment(PRUint32 aLangID) = 0;
  /**
   * Get a script context (WITHOUT added reference) for the specified language.
   */
  virtual nsIScriptContext *GetScriptContext(PRUint32 lang) = 0;
  
  /**
   * Get the opaque "global" object for the specified lang.
   */
  virtual void *GetScriptGlobal(PRUint32 lang) = 0;

  // Set/GetContext deprecated methods - use GetScriptContext/Global
  virtual JSObject *GetGlobalJSObject() {
        return (JSObject *)GetScriptGlobal(nsIProgrammingLanguage::JAVASCRIPT);
  }

  virtual nsIScriptContext *GetContext() {
        return GetScriptContext(nsIProgrammingLanguage::JAVASCRIPT);
  }

  /**
   * Set a new language context for this global.  The native global for the
   * context is created by the context's GetNativeGlobal() method.
   */

  virtual nsresult SetScriptContext(PRUint32 lang, nsIScriptContext *aContext) = 0;

  /**
   * Called when the global script for a language is finalized, typically as
   * part of its GC process.  By the time this call is made, the
   * nsIScriptContext for the language has probably already been removed.
   * After this call, the passed object is dead - which should generally be the
   * same object the global is using for a global for that language.
   */

  virtual void OnFinalize(PRUint32 aLangID, void *aScriptGlobal) = 0;

  /**
   * Called to enable/disable scripts.
   */
  virtual void SetScriptsEnabled(PRBool aEnabled, PRBool aFireTimeouts) = 0;

  /**
   * Handle a script error.  Generally called by a script context.
   */
  virtual nsresult HandleScriptError(nsScriptErrorEvent *aErrorEvent,
                                     nsEventStatus *aEventStatus) {
    return NS_HandleScriptError(this, aErrorEvent, aEventStatus);
  }
};

//NS_DEFINE_STATIC_IID_ACCESSOR(nsIScriptGlobalObject,
//                              NS_ISCRIPTGLOBALOBJECT_IID)


// 87482b5e-e019-4df5-9bc2-b2a51b1f2d28
#define NS_ISCRIPTCONTEXT_IID \
{ 0x87482b5e, 0xe019, 0x4df5, \
  { 0x9b, 0xc2, 0xb2, 0xa5, 0x1b, 0x1f, 0x2d, 0x28 } }

class nsIAtom;
class nsIScriptContextOwner;
typedef void (*nsScriptTerminationFunc)(nsISupports* ref);


class nsIScriptGlobalObject;
class nsIScriptSecurityManager;
class nsIPrincipal;
class nsIAtom;
class nsIArray;
class nsIVariant;
class nsIObjectInputStream;
class nsIObjectOutputStream;
class nsScriptObjectHolder;


/**
 * It is used by the application to initialize a runtime and run scripts.
 * A script runtime would implement this interface.
 * <P><I>It does have a bit too much java script information now, that
 * should be removed in a short time. Ideally this interface will be
 * language neutral</I>
 */
class nsIScriptContext : public nsISupports
{
public:
NS_DEFINE_STATIC_IID_ACCESSOR(NS_ISCRIPTCONTEXT_IID)
//  NS_DECLARE_STATIC_IID_ACCESSOR(NS_ISCRIPTCONTEXT_IID)

  /* Get the ID of this language. */
  virtual PRUint32 GetScriptTypeID() = 0;

  /**
   * Compile and execute a script.
   *
   * @param aScript a string representing the script to be executed
   * @param aScopeObject a script object for the scope to execute in, or
   *                     nsnull to use a default scope
   * @param aPrincipal the principal that produced the script
   * @param aURL the URL or filename for error messages
   * @param aLineNo the starting line number of the script for error messages
   * @param aVersion the script language version to use when executing
   * @param aRetValue the result of executing the script, or null for no result.
   *        If this is a JS context, it's the caller's responsibility to
   *        preserve aRetValue from GC across this call
   * @param aIsUndefined true if the result of executing the script is the
   *                     undefined value
   *
   * @return NS_OK if the script was valid and got executed
   *
   **/
  virtual nsresult EvaluateString(const nsAString& aScript,
                                  void *aScopeObject,
                                  nsIPrincipal *aPrincipal,
                                  const char *aURL,
                                  PRUint32 aLineNo,
                                  PRUint32 aVersion,
                                  nsAString *aRetValue,
                                  PRBool* aIsUndefined) = 0;

  // Note JS bigotry remains here - 'void *aRetValue' is assumed to be a
  // jsval.  This must move to JSObject before it can be made agnostic.
  virtual nsresult EvaluateStringWithValue(const nsAString& aScript,
                                           void *aScopeObject,
                                           nsIPrincipal *aPrincipal,
                                           const char *aURL,
                                           PRUint32 aLineNo,
                                           PRUint32 aVersion,
                                           void* aRetValue,
                                           PRBool* aIsUndefined) = 0;

  /**
   * Compile a script.
   *
   * @param aText a PRUnichar buffer containing script source
   * @param aTextLength number of characters in aText
   * @param aScopeObject an object telling the scope in which to execute,
   *                     or nsnull to use a default scope
   * @param aPrincipal the principal that produced the script
   * @param aURL the URL or filename for error messages
   * @param aLineNo the starting line number of the script for error messages
   * @param aVersion the script language version to use when executing
   * @param aScriptObject an executable object that's the result of compiling
   *                      the script.
   *
   * @return NS_OK if the script source was valid and got compiled.
   *
   **/
  virtual nsresult CompileScript(const PRUnichar* aText,
                                 PRInt32 aTextLength,
                                 void* aScopeObject,
                                 nsIPrincipal* aPrincipal,
                                 const char* aURL,
                                 PRUint32 aLineNo,
                                 PRUint32 aVersion,
                                 nsScriptObjectHolder &aScriptObject) = 0;

  /**
   * Execute a precompiled script object.
   *
   * @param aScriptObject an object representing the script to be executed
   * @param aScopeObject an object telling the scope in which to execute,
   *                     or nsnull to use a default scope
   * @param aRetValue the result of executing the script, may be null in
   *                  which case no result string is computed
   * @param aIsUndefined true if the result of executing the script is the
   *                     undefined value, may be null for "don't care"
   *
   * @return NS_OK if the script was valid and got executed
   *
   */
  virtual nsresult ExecuteScript(void* aScriptObject,
                                 void* aScopeObject,
                                 nsAString* aRetValue,
                                 PRBool* aIsUndefined) = 0;

  /**
   * Compile the event handler named by atom aName, with function body aBody
   * into a function object returned if ok via aHandler.  Does NOT bind the
   * function to anything - BindCompiledEventHandler() should be used for that
   * purpose.  Note that this event handler is always considered 'shared' and
   * hence is compiled without principals.  Never call the returned object
   * directly - it must be bound (and thereby cloned, and therefore have the 
   * correct principals) before use!
   *
   * If the compilation sets a pending exception on the native context, it is
   * this method's responsibility to report said exception immediately, without
   * relying on callers to do so.
   *
   *
   * @param aName an nsIAtom pointer naming the function; it must be lowercase
   *        and ASCII, and should not be longer than 63 chars.  This bound on
   *        length is enforced only by assertions, so caveat caller!
   * @param aEventName the name that the event object should be bound to
   * @param aBody the event handler function's body
   * @param aURL the URL or filename for error messages
   * @param aLineNo the starting line number of the script for error messages
   * @param aVersion the script language version to use when executing
   * @param aHandler the out parameter in which a void pointer to the compiled
   *        function object is stored on success
   *
   * @return NS_OK if the function body was valid and got compiled
   */
  virtual nsresult CompileEventHandler(nsIAtom* aName,
                                       PRUint32 aArgCount,
                                       const char** aArgNames,
                                       const nsAString& aBody,
                                       const char* aURL,
                                       PRUint32 aLineNo,
                                       PRUint32 aVersion,
                                       nsScriptObjectHolder &aHandler) = 0;

  /**
   * Call the function object with given args and return its boolean result,
   * or true if the result isn't boolean.
   *
   * @param aTarget the event target
   * @param aScript an object telling the scope in which to call the compiled
   *        event handler function.
   * @param aHandler function object (function and static scope) to invoke.
   * @param argv array of arguments.  Note each element is assumed to
   *        be an nsIVariant.
   * @param rval out parameter returning result
   **/
  virtual nsresult CallEventHandler(nsISupports* aTarget,
                                    void *aScope, void* aHandler,
                                    nsIArray *argv, nsIVariant **rval) = 0;

  /**
   * Bind an already-compiled event handler function to a name in the given
   * scope object.  The same restrictions on aName (lowercase ASCII, not too
   * long) applies here as for CompileEventHandler.  Scripting languages with
   * static scoping must re-bind the scope chain for aHandler to begin (after
   * the activation scope for aHandler itself, typically) with aTarget's scope.
   *
   * Logically, this 'bind' operation is more of a 'copy' - it simply
   * stashes/associates the event handler function with the event target, so
   * it can be fetched later with GetBoundEventHandler().
   *
   * @param aTarget an object telling the scope in which to bind the compiled
   *        event handler function.  The context will presumably associate
   *        this nsISupports with a native script object.
   * @param aName an nsIAtom pointer naming the function; it must be lowercase
   *        and ASCII, and should not be longer than 63 chars.  This bound on
   *        length is enforced only by assertions, so caveat caller!
   * @param aHandler the function object to name, created by an earlier call to
   *        CompileEventHandler
   * @return NS_OK if the function was successfully bound to the name
   *
   * XXXmarkh - fold this in with SetProperty?  Exactly the same concept!
   */
  virtual nsresult BindCompiledEventHandler(nsISupports* aTarget, void *aScope,
                                            nsIAtom* aName,
                                            void* aHandler) = 0;

  /**
   * Lookup a previously bound event handler for the specified target.  This
   * will return an object equivilent to the one passed to
   * BindCompiledEventHandler (although the pointer may not be the same).
   *
   */
  virtual nsresult GetBoundEventHandler(nsISupports* aTarget, void *aScope,
                                        nsIAtom* aName,
                                        nsScriptObjectHolder &aHandler) = 0;

  /**
   * Compile a function that isn't used as an event handler.
   *
   * NOTE: Not yet language agnostic (main problem is XBL - not yet agnostic)
   * Caller must make sure aFunctionObject is a JS GC root.
   *
   **/
  virtual nsresult CompileFunction(void* aTarget,
                                   const nsACString& aName,
                                   PRUint32 aArgCount,
                                   const char** aArgArray,
                                   const nsAString& aBody,
                                   const char* aURL,
                                   PRUint32 aLineNo,
                                   PRUint32 aVersion,
                                   PRBool aShared,
                                   void **aFunctionObject) = 0;

  /**
   * Set the default scripting language version for this context, which must
   * be a context specific to a particular scripting language.
   *
   **/
  virtual void SetDefaultLanguageVersion(PRUint32 aVersion) = 0;

  /**
   * Return the global object.
   *
   **/
  virtual nsIScriptGlobalObject *GetGlobalObject() = 0;

  /**
   * Return the native script context
   *
   **/
  virtual void *GetNativeContext() = 0;

  /**
   * Return the native global object for this context.
   *
   **/
  virtual void *GetNativeGlobal() = 0;

  /**
   * Create a new global object that will be used for an inner window.
   * Return the native global and an nsISupports 'holder' that can be used
   * to manage the lifetime of it.
   */
  virtual nsresult CreateNativeGlobalForInner(
                                      nsIScriptGlobalObject *aNewInner,
                                      PRBool aIsChrome,
                                      void **aNativeGlobal,
                                      nsISupports **aHolder) = 0;

  /**
   * Connect this context to a new inner window, to allow "prototype"
   * chaining from the inner to the outer.
   * Called after both the the inner and outer windows are initialized
   **/
  virtual nsresult ConnectToInner(nsIScriptGlobalObject *aNewInner,
                                  void *aOuterGlobal) = 0;


  /**
   * Init this context ready for use.  If aGlobalObject is not NULL, this
   * function may initialize based on this global (for example, using the
   * global to locate a chrome window, create a new 'scope' for this
   * global, etc)
   *
   * @param aGlobalObject the gobal object, which may be nsnull.
   *
   * @return NS_OK if context initialization was successful
   *
   **/
  virtual nsresult InitContext(nsIScriptGlobalObject *aGlobalObject) = 0;

  /**
   * Check to see if context is as yet intialized. Used to prevent
   * reentrancy issues during the initialization process.
   *
   * @return PR_TRUE if initialized, PR_FALSE if not
   *
   */
  virtual PRBool IsContextInitialized() = 0;

  /**
   * Called as the global object discards its reference to the context.
   */
  virtual void FinalizeContext() = 0;

  /**
   * For garbage collected systems, do a synchronous collection pass.
   * May be a no-op on other systems
   *
   * @return NS_OK if the method is successful
   */
  virtual void GC() = 0;

  /**
   * Inform the context that a script was evaluated.
   * A GC may be done if "necessary."
   * This call is necessary if script evaluation is done
   * without using the EvaluateScript method.
   * @param aTerminated If true then call termination function if it was 
   *    previously set. Within DOM this will always be true, but outside 
   *    callers (such as xpconnect) who may do script evaluations nested
   *    inside DOM script evaluations can pass false to avoid premature
   *    calls to the termination function.
   * @return NS_OK if the method is successful
   */
  virtual void ScriptEvaluated(PRBool aTerminated) = 0;

  virtual nsresult Serialize(nsIObjectOutputStream* aStream,
                             void *aScriptObject) = 0;
  
  /* Deserialize a script from a stream.
   */
  virtual nsresult Deserialize(nsIObjectInputStream* aStream,
                               nsScriptObjectHolder &aResult) = 0;

  /**
   * JS only - this function need not be implemented by languages other
   * than JS (ie, this should be moved to a private interface!)
   * Called to specify a function that should be called when the current
   * script (if there is one) terminates. Generally used if breakdown
   * of script state needs to happen, but should be deferred till
   * the end of script evaluation.
   *
   * @throws NS_ERROR_OUT_OF_MEMORY if that happens
   */
  virtual nsresult SetTerminationFunction(nsScriptTerminationFunc aFunc,
                                          nsISupports* aRef) = 0;

  /**
   * Called to disable/enable script execution in this context.
   */
  virtual PRBool GetScriptsEnabled() = 0;
  virtual void SetScriptsEnabled(PRBool aEnabled, PRBool aFireTimeouts) = 0;

  // SetProperty is suspect and jst believes should not be needed.  Currenly
  // used only for "arguments".
  virtual nsresult SetProperty(void *aTarget, const char *aPropName, nsISupports *aVal) = 0;
  /** 
   * Called to set/get information if the script context is
   * currently processing a script tag
   */
  virtual PRBool GetProcessingScriptTag() = 0;
  virtual void SetProcessingScriptTag(PRBool aResult) = 0;

  /**
   * Tell the context whether or not to GC when destroyed.  An optimization
   * used when the window is a [i]frame, so GC will happen anyway.
   */
  virtual void SetGCOnDestruction(PRBool aGCOnDestruction) = 0;

  /**
   * Initialize DOM classes on aGlobalObj, always call
   * WillInitializeContext() before calling InitContext(), and always
   * call DidInitializeContext() when a context is fully
   * (successfully) initialized.
   */
  virtual nsresult InitClasses(void *aGlobalObj) = 0;

  /**
   * Clear the scope object - may be called either as we are being torn down,
   * or before we are attached to a different document.
   *
   * aClearFromProtoChain is probably somewhat JavaScript specific.  It
   * indicates that the global scope polluter should be removed from the
   * prototype chain and that the objects in the prototype chain should
   * also have their scopes cleared.  We don't do this all the time
   * because the prototype chain is shared between inner and outer
   * windows, and needs to stay with inner windows that we're keeping
   * around.
   */
  virtual void ClearScope(void* aGlobalObj, PRBool aClearFromProtoChain) = 0;

  /**
   * Tell the context we're about to be reinitialize it.
   */
  virtual void WillInitializeContext() = 0;

  /**
   * Tell the context we're done reinitializing it.
   */
  virtual void DidInitializeContext() = 0;

  /**
   * Tell the context our global has a new document, and the scope
   * used by it.  Use nsISupports to avoid dependency issues - but expect
   * a QI for nsIDOMDocument and/or nsIDocument.
   */
  virtual void DidSetDocument(nsISupports *aDoc, void *aGlobal) = 0;

  /* Memory managment for script objects.  Used by the implementation of
   * nsScriptObjectHolder to manage the lifetimes of the held script objects.
   *
   * See also nsIScriptRuntime, which has identical methods and is useful
   * in situations when you do not have an nsIScriptContext.
   * 
   */
  virtual nsresult DropScriptObject(void *object) = 0;
  virtual nsresult HoldScriptObject(void *object) = 0;

  virtual void EnterModalState() = 0;
  virtual void LeaveModalState() = 0;
};





bool wxWebControl::Execute(const wxString& js_code)
{
    wxString result;
    return ExecuteJSCode(js_code, result);
}

wxString wxWebControl::ExecuteScriptWithResult(const wxString& js_code)
{
    wxString result;
    ExecuteJSCode(js_code, result);
    return result;
}

bool wxWebControl::ExecuteJSCode(const wxString& js_code, wxString& result)
{
    nsresult rv;

    ns_smartptr<nsIScriptSecurityManager> security_manager;
    security_manager = nsGetService("@mozilla.org/scriptsecuritymanager;1");
    if (security_manager.empty())
        return false;
    
    ns_smartptr<nsIPrincipal> principal;
    security_manager->GetSystemPrincipal(&principal.p);
    if (principal.empty())
        return false;
    
    ns_smartptr<nsIScriptGlobalObject> sgo = nsRequestInterface(m_ptrs->m_web_browser);
    if (sgo.empty())
        return false;
    ns_smartptr<nsIScriptContext> ctx = sgo->GetContext();
    if (ctx.empty())
        return false;

    void* obj = sgo->GetGlobalJSObject();
    
    nsEmbedString str;
    wx2ns(js_code, str);

    jsval out;
    rv = ctx->EvaluateStringWithValue(
        str,
        sgo->GetGlobalJSObject(),
        principal,
        "mozembed",
        0,
        nsnull,
        &out,
        nsnull);
    if (NS_FAILED(rv))  {
        return false;
    }
    // XXX: Root this variable properly to prevent GC?
    JSString *jsstring = JS_ValueToString((JSContext*)ctx->GetNativeContext(), out);
    if (jsstring)  {
        wxString _wxString(JS_GetStringBytes(jsstring), wxConvUTF8);
        result = _wxString;
    }

    return true;
}

// GeckoEngine is an internal class wchich manages the xulrunner engine;
// It does not need to be called publicly

class GeckoEngine
{
public:

    GeckoEngine();
    ~GeckoEngine();
    
    void SetEnginePath(const wxString& path);
    void SetStoragePath(const wxString& path);
    
    bool Init();
    bool IsOk() const;
    
    void AddContentListener(ContentListener* l);
    ContentListenerPtrArray& GetContentListeners();

    void AddPluginPath(const wxString& path);
    
    // xulrunner versions 1.8 will return true, 1.9 will return false
    bool IsVersion18() const { return m_is18; }
    
private:

    wxString m_gecko_path;
    wxString m_storage_path;
    wxString m_history_filename;
    bool m_ok;
    bool m_is18;
    
    ContentListenerPtrArray m_content_listeners;
    ns_smartptr<nsIAppShell> m_appshell;
    PluginListProvider* m_plugin_provider;
};


// global instance of the GeckoEngine object

GeckoEngine g_gecko_engine;




///////////////////////////////////////////////////////////////////////////////
//  browser chrome implementation
///////////////////////////////////////////////////////////////////////////////


// this interface allows us to get the wxWebControl
// pointer from a browser chrome object in a safe way

#define NS_ICHROMEINTERNAL_IID \
  {0x7fe3c660, 0x376c, 0x43e9, \
  { 0x9f, 0xdd, 0x69, 0x85, 0x4a, 0xfe, 0xc9, 0x46 }}
  

class NS_NO_VTABLE nsIChromeInternal : public nsISupports
{
public: 

    NS_DEFINE_STATIC_IID_ACCESSOR(NS_ICHROMEINTERNAL_IID)

    virtual wxWebControl* GetWebControl() = 0;
};


class BrowserChrome : public nsIWebBrowserChrome,
                      public nsIChromeInternal,
                      public nsIWebBrowserChromeFocus,
                      public nsIWebProgressListener,
                      public nsIEmbeddingSiteWindow2,
                      public nsIInterfaceRequestor,
                      public nsSupportsWeakReference,
                      public nsIContextMenuListener2,
                      public nsITooltipListener,
                      public nsIDOMEventListener
{
public:

    NS_DECL_ISUPPORTS
    NS_DECL_NSIWEBBROWSERCHROME
    NS_DECL_NSIWEBBROWSERCHROMEFOCUS
    NS_DECL_NSIWEBPROGRESSLISTENER
    NS_DECL_NSIINTERFACEREQUESTOR
    NS_DECL_NSIEMBEDDINGSITEWINDOW
    NS_DECL_NSIEMBEDDINGSITEWINDOW2
    NS_DECL_NSICONTEXTMENULISTENER2
    NS_DECL_NSITOOLTIPLISTENER
    NS_DECL_NSIDOMEVENTLISTENER

    BrowserChrome(wxWebControl* wnd);
    virtual ~BrowserChrome();
    
    void ChromeInit();
    void ChromeUninit();
    
    wxWebControl* GetWebControl() { return m_wnd; }
    
public:
    ns_smartptr<nsIWebBrowser> m_web_browser;
    wxWebControl* m_wnd;
    wxString m_title;
    PRUint32 m_chrome_mask;
    wxDialog* m_dialog;
    nsresult m_dialog_retval;
};


static nsIDOMNode* GetAnchor(nsIDOMNode* node)
{
    // note: this function finds if there's an anchor element
    // in the parent hierarchy, and returns it if it exists;
    // otherwise, it returns null

    if (!node)
        return node;

    ns_smartptr<nsIDOMNode> n = node;
    ns_smartptr<nsIDOMHTMLAnchorElement> anchor;
    anchor = n;

    if (anchor)
        return node;

    node->GetParentNode(&node);
    return GetAnchor(node);
}


NS_IMPL_ADDREF(BrowserChrome)
NS_IMPL_RELEASE(BrowserChrome)

NS_INTERFACE_MAP_BEGIN(BrowserChrome)
    NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIWebBrowserChrome)
    NS_INTERFACE_MAP_ENTRY(nsIWebBrowserChrome)
    NS_INTERFACE_MAP_ENTRY(nsIChromeInternal)
    NS_INTERFACE_MAP_ENTRY(nsIWebBrowserChromeFocus)
    NS_INTERFACE_MAP_ENTRY(nsIWebProgressListener)
    NS_INTERFACE_MAP_ENTRY(nsIEmbeddingSiteWindow)
    NS_INTERFACE_MAP_ENTRY(nsIEmbeddingSiteWindow2)
    NS_INTERFACE_MAP_ENTRY(nsIInterfaceRequestor)
    NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
    NS_INTERFACE_MAP_ENTRY(nsIContextMenuListener2)
    NS_INTERFACE_MAP_ENTRY(nsITooltipListener)
    NS_INTERFACE_MAP_ENTRY(nsIDOMEventListener)
NS_INTERFACE_MAP_END


BrowserChrome::BrowserChrome(wxWebControl* wnd)
{
    m_wnd = wnd;
    m_chrome_mask = nsIWebBrowserChrome::CHROME_ALL;
    m_dialog = NULL;
    m_dialog_retval = NS_OK;
}

BrowserChrome::~BrowserChrome()
{
}

void BrowserChrome::ChromeInit()
{
    nsresult res;

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("keydown"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("keyup"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("mousedown"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("mouseup"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("mouseover"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("mouseout"),
                                            this,
                                            PR_TRUE);
                                                   
    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("dblclick"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("dragdrop"),
                                            this,
                                            PR_FALSE);
                                 
    // these two event types are used to capture favicon information
    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("DOMLinkAdded"),
                                            this,
                                            PR_FALSE);
                                            
    res = m_wnd->m_ptrs->m_event_target->AddEventListener(
                                            NS_LITERAL_STRING("DOMContentLoaded"),
                                            this,
                                            PR_FALSE);
}

void BrowserChrome::ChromeUninit()
{
    nsresult res;

    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("keydown"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("keyup"),
                                            this,
                                            PR_TRUE);
    
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("mousedown"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("mouseup"),
                                            this,
                                            PR_TRUE);
    
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("mouseover"),
                                            this,
                                            PR_TRUE);

    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("mouseout"),
                                            this,
                                            PR_TRUE);
                                                   
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("dblclick"),
                                            this,
                                            PR_TRUE);
                                            
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("dragdrop"),
                                            this,
                                            PR_TRUE);
                                            
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("DOMLinkAdded"),
                                            this,
                                            PR_FALSE);
                                            
    res = m_wnd->m_ptrs->m_event_target->RemoveEventListener(
                                            NS_LITERAL_STRING("DOMContentLoaded"),
                                            this,
                                            PR_FALSE);
    
    m_wnd = NULL;
}

NS_IMETHODIMP BrowserChrome::GetWebBrowser(nsIWebBrowser** web_browser)
{
    NS_ENSURE_ARG_POINTER(web_browser);
    *web_browser = m_web_browser;
    NS_IF_ADDREF(*web_browser);
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetWebBrowser(nsIWebBrowser* web_browser)
{
    NS_ENSURE_ARG_POINTER(web_browser);
    m_web_browser = web_browser;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::FocusNextElement()
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP BrowserChrome::FocusPrevElement()
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP BrowserChrome::GetChromeFlags(PRUint32* chrome_mask)
{
    NS_ENSURE_ARG_POINTER(chrome_mask);
    *chrome_mask = m_chrome_mask;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetChromeFlags(PRUint32 chrome_mask)
{
    m_chrome_mask = chrome_mask;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SizeBrowserTo(PRInt32 aCX, PRInt32 aCY)
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::ShowAsModal()
{
    if (!m_wnd)
        return NS_OK;
  
    wxWindow* parent = m_wnd->GetParent();
    wxDialog* dialog = NULL;
    while (parent)
    {
        if (parent->IsKindOf(CLASSINFO(wxDialog)))
        {
            dialog = (wxDialog*)parent;
            break;
        }
        
        parent = parent->GetParent();
    }
    
    if (!dialog)
        return NS_OK;
 
    m_dialog = dialog;
    m_dialog->ShowModal();
    m_dialog = NULL;

    return m_dialog_retval;
}

NS_IMETHODIMP BrowserChrome::IsWindowModal(PRBool* retval)
{
    *retval = m_dialog ? PR_TRUE : PR_FALSE;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::ExitModalEventLoop(nsresult status)
{
    if (m_dialog == NULL)
        return NS_OK;
    m_dialog_retval = status;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetFocus()
{
    m_wnd->m_ptrs->m_base_window->SetFocus();
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::GetTitle(PRUnichar** title)
{
    *title = wxToUnichar(m_title);
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetTitle(const PRUnichar* title)
{
    if (!m_wnd)
        return NS_OK;

    m_title = ns2wx(title);
    
    if (m_dialog)
    {
        // modal dialogs we do ourself
        m_dialog->SetTitle(ns2wx(title));
        return NS_OK;
    }
      
    wxWebEvent evt(wxEVT_WEB_TITLECHANGE, m_wnd->GetId());
    evt.SetEventObject(m_wnd);
    evt.SetString(m_title);
    m_wnd->GetEventHandler()->ProcessEvent(evt);
    
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::GetVisibility(PRBool* visibility)
{
    *visibility = PR_TRUE;
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetVisibility(PRBool visibility)
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::DestroyBrowserWindow()
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetStatus(PRUint32 type, const PRUnichar* status)
{
    if (!m_wnd)
        return NS_OK;

    if (type == STATUS_LINK)
    {
        wxWebEvent evt(wxEVT_WEB_STATUSTEXT, m_wnd->GetId());
        evt.SetEventObject(m_wnd);
        evt.SetString(ns2wx(status));
        m_wnd->GetEventHandler()->ProcessEvent(evt);
    }
      
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::SetDimensions(PRUint32 flags,
                                           PRInt32 x, PRInt32 y,
                                           PRInt32 cx, PRInt32 cy)
{
    return NS_OK;
}


NS_IMETHODIMP BrowserChrome::GetDimensions(PRUint32 flags,
                                           PRInt32* x, PRInt32* y,
                                           PRInt32* cx, PRInt32* cy)
{
    if (!m_wnd)
    {
        // no window (almost never happens)
        *x = 0; *y = 0;
        *cx = 100; *cy = 100;
        return NS_OK;
    }

    wxPoint pos = m_wnd->GetPosition();
    wxSize size = m_wnd->GetSize();

    if (x)
        *x = pos.x;
    if (y)
        *y = pos.y;
    if (cx)
        *cx = size.x;
    if (cy)
        *cy = size.y;

    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::GetSiteWindow(void** site_window)
{
    NS_ENSURE_ARG_POINTER(site_window);
    #ifdef __WXGTK_
    *site_window = (void*)m_wnd->m_wxwindow;
    #else
    *site_window = (void*)m_wnd->GetHandle();
    #endif
    return NS_OK;
}

// nsIInterfaceRequestor::GetInterface()
NS_IMETHODIMP BrowserChrome::GetInterface(const nsIID& IID, void** instance_ptr)
{
    if(IID.Equals(NS_GET_IID(nsIDOMWindow)))
    {
        if (m_web_browser)
            return m_web_browser->GetContentDOMWindow((nsIDOMWindow**)instance_ptr);
        return NS_ERROR_NOT_INITIALIZED;
    }

    return QueryInterface(IID, instance_ptr);
}

// nsIWebProgressListener::OnProgressChange()
NS_IMETHODIMP BrowserChrome::OnProgressChange(nsIWebProgress* progress,
                                              nsIRequest* request,
                                              PRInt32 curSelfProgress,
                                              PRInt32 maxSelfProgress,
                                              PRInt32 curTotalProgress,
                                              PRInt32 maxTotalProgress)
{
    return NS_OK;
}

// nsIWebProgressListener::OnStateChange()
NS_IMETHODIMP BrowserChrome::OnStateChange(nsIWebProgress* progress,
                                           nsIRequest* request,
                                           PRUint32 progress_state_flags,
                                           nsresult status)
{
    if (!m_wnd)
        return NS_OK;

    wxWebEvent evt(wxEVT_WEB_STATECHANGE, m_wnd->GetId());
    evt.SetEventObject(m_wnd);
    
    int state = wxWEB_STATE_NONE;
    int res = wxWEB_RESULT_NONE;
    
    if (progress_state_flags & STATE_STOP)
        state |= wxWEB_STATE_STOP;
    if (progress_state_flags & STATE_START)
        state |= wxWEB_STATE_START;
    if (progress_state_flags & STATE_REDIRECTING)
        state |= wxWEB_STATE_REDIRECTING;
    if (progress_state_flags & STATE_TRANSFERRING)
        state |= wxWEB_STATE_TRANSFERRING;
    if (progress_state_flags & STATE_NEGOTIATING)
        state |= wxWEB_STATE_NEGOTIATING;

    if (progress_state_flags & STATE_IS_REQUEST)
        state |= wxWEB_STATE_IS_REQUEST;
    if (progress_state_flags & STATE_IS_DOCUMENT)
        state |= wxWEB_STATE_IS_DOCUMENT;
    if (progress_state_flags & STATE_IS_NETWORK)
        state |= wxWEB_STATE_IS_NETWORK;
    if (progress_state_flags & STATE_IS_WINDOW)
        state |= wxWEB_STATE_IS_WINDOW;
    
    /*
    if (status == NS_OK)
        res = wxWEB_RESULT_SUCCESS;
     else if (status == NS_ERROR_UNKNOWN_PROTOCOL)
        res = wxWEB_RESULT_UNKNOWN_PROTOCOL;
     else if (status == NS_ERROR_FILE_NOT_FOUND)
        res = wxWEB_RESULT_FILE_NOT_FOUND;
     else if (status == NS_ERROR_UNKNOWN_HOST)
        res = wxWEB_RESULT_UNKNOWN_HOST;
     else if (status == NS_ERROR_CONNECTION_REFUSED)
        res = wxWEB_RESULT_CONNECTION_REFUSED;
     else if (status == NS_ERROR_NET_INTERRUPT)
        res = wxWEB_RESULT_NET_INTERRUPT;
     else if (status == NS_ERROR_NET_TIMEOUT)
        res = wxWEB_RESULT_NET_TIMEOUT;
     else if (status == NS_ERROR_MALFORMED_URI)
        res = wxWEB_RESULT_MALFORMED_URI;
     else if (status == NS_ERROR_REDIRECT_LOOP)
        res = wxWEB_RESULT_REDIRECT_LOOP;
     else if (status == NS_ERROR_UNKNOWN_SOCKET_TYPE)
        res = wxWEB_RESULT_UNKNOWN_SOCKET_TYPE;
     else if (status == NS_ERROR_NET_RESET)
        res = wxWEB_RESULT_NET_RESET;
     else if (status == NS_ERROR_DOCUMENT_NOT_CACHED)
        res = wxWEB_RESULT_DOCUMENT_NOT_CACHED;
     else if (status == NS_ERROR_DOCUMENT_IS_PRINTMODE)
        res = wxWEB_RESULT_DOCUMENT_IS_PRINTMODE;
     else if (status == NS_ERROR_PORT_ACCESS_NOT_ALLOWED)
        res = wxWEB_RESULT_PORT_ACCESS_NOT_ALLOWED;
     else if (status == NS_ERROR_UNKNOWN_PROXY_HOST)
        res = wxWEB_RESULT_UNKNOWN_PROXY_HOST;
     else if (status == NS_ERROR_PROXY_CONNECTION_REFUSED)
        res = wxWEB_RESULT_PROXY_CONNECTION_REFUSED;
    */
        
    evt.SetState(state);
    evt.SetResult(res);
    m_wnd->GetEventHandler()->ProcessEvent(evt);
    
    return NS_OK;
}

// nsIWebProgressListener::OnLocationChange()
NS_IMETHODIMP BrowserChrome::OnLocationChange(nsIWebProgress* progress,
                                              nsIRequest* request,
                                              nsIURI* location)
{
    if (!m_wnd)
        return NS_OK;

    nsEmbedCString url;
    location->GetSpec(url);

    wxWebEvent evt(wxEVT_WEB_LOCATIONCHANGE, m_wnd->GetId());
    evt.SetEventObject(m_wnd);
    evt.SetString(ns2wx(url));
    m_wnd->GetEventHandler()->ProcessEvent(evt);
    
    return NS_OK;
}

// nsIWebProgressListener::OnStatusChange()
NS_IMETHODIMP BrowserChrome::OnStatusChange(nsIWebProgress* progress,
                                            nsIRequest* request,
                                            nsresult status,
                                            const PRUnichar* message)
{
    if (!m_wnd)
        return NS_OK;

    wxWebEvent evt(wxEVT_WEB_STATUSCHANGE, m_wnd->GetId());
    evt.SetEventObject(m_wnd);
    evt.SetString(ns2wx(message));
    m_wnd->GetEventHandler()->ProcessEvent(evt);

    return NS_OK;
}

// nsIWebProgressListener::OnSecurityChange()
NS_IMETHODIMP BrowserChrome::OnSecurityChange(nsIWebProgress* progress,
                                              nsIRequest* request,
                                              PRUint32 state)
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::OnShowContextMenu(PRUint32 context_flags,
                                               nsIContextMenuInfo* info)
{
    if (!m_wnd)
        return NS_OK;

    wxWebEvent evt(wxEVT_WEB_SHOWCONTEXTMENU, m_wnd->GetId());
    
    ns_smartptr<nsIDOMEvent> mouse_event;
    info->GetMouseEvent(&mouse_event.p);
    
    if (mouse_event)
    {
        ns_smartptr<nsIDOMEventTarget> target;
        mouse_event->GetTarget(&target.p);
        
        evt.m_target_node.m_data->setNode(target);

        // note: following parallels code in mouse event; see if the target
        // or any of its parent nodes are anchors and if so, set the event href;
        // the reason we have to look through the parents of the target is
        // because the target may be a child node of the element where the
        // actual href is specified, and as a result, may not itself contain
        // the href; this happens, for example, when portions of the text in a
        // hyperlink are bold
        ns_smartptr<nsIDOMNode> node;
        node = target;
        node.p = GetAnchor(node.p);
        
        ns_smartptr<nsIDOMHTMLAnchorElement> anchor;
        anchor = node;

        if (anchor)
        {
            nsEmbedString nss;
            anchor->GetHref(nss);
            evt.SetHref(ns2wx(nss));
        }
    }

    evt.SetEventObject(m_wnd);
    m_wnd->GetEventHandler()->ProcessEvent(evt);
    
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::OnShowTooltip(PRInt32 x,
                                           PRInt32 y,
                                           const PRUnichar* tip_text)
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::OnHideTooltip()
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::Blur()
{
    return NS_OK;
}

NS_IMETHODIMP BrowserChrome::HandleEvent(nsIDOMEvent* evt)
{
    if (!m_wnd)
        return NS_OK;

    nsEmbedString nsstr_type;
    evt->GetType(nsstr_type);
    wxString type = ns2wx(nsstr_type);

    if (type == wxT("dragdrop"))
    {

    }
    
    
    if (type == wxT("DOMContentLoaded"))
    {
        m_wnd->OnDOMContentLoaded();
        
        ns_smartptr<nsIURI> uri, uri2;
        m_wnd->m_ptrs->m_web_navigation->GetCurrentURI(&uri.p);
        if (!uri)
            return NS_OK;
    
        // skip https
        PRBool b = PR_FALSE;
        uri->SchemeIs("https", &b);
        if (b)
            return NS_OK;
        uri->SchemeIs("file", &b);
        if (b)
            return NS_OK;
            
        nsEmbedCString url_str;
        uri->Resolve(NS_LITERAL_CSTRING("/favicon.ico"), url_str);
        
        uri2 = nsNewURI(ns2wx(url_str));
        if (!uri2)
            return NS_OK;
            
        // let main control know that we should have a favicon
        // by now.  If we don't, load a default /favicon.ico
        m_wnd->FetchFavIcon((void*)uri2.p);
        
    }
    
    if (type == wxT("DOMLinkAdded"))
    {
        ns_smartptr<nsIDOMEventTarget> target;
        evt->GetTarget(&target.p);
        
        ns_smartptr<nsIDOMElement> element = target;
        if (!element)
            return NS_OK;
        
        // make sure we're dealing with a link tag
        nsEmbedString value;
        wxString tagname, rel, href, spec;
        
        element->GetTagName(value);
        tagname = ns2wx(value);
        tagname.MakeLower();
        if (tagname != wxT("link"))
            return NS_OK;
            
        // make sure it has a rel attribute
        element->GetAttribute(NS_LITERAL_STRING("rel"), value);
        rel = ns2wx(value);
        rel.MakeLower();
        
        if (rel != wxT("shortcut icon") &&
            rel != wxT("icon"))
        {
            return NS_OK;
        }
        
        // get the href attribute
        element->GetAttribute(NS_LITERAL_STRING("href"), value);
        href = ns2wx(value);
       
        // now get the dom document
        ns_smartptr<nsIDOMDocument> dom_doc;
        element->GetOwnerDocument(&dom_doc.p);
        ns_smartptr<nsIDOM3Document> dom3_doc = dom_doc;
        if (!dom3_doc)
            return NS_OK;
        
        dom3_doc->GetDocumentURI(value);
        spec = ns2wx(value);
        
        nsEmbedCString chref;
        nsEmbedCString cvalue;
        wx2ns(href, chref);
        ns_smartptr<nsIURI> doc_uri = nsNewURI(spec);
        doc_uri->Resolve(chref, cvalue);
        
        wxString favicon_url = ns2wx(cvalue);
        ns_smartptr<nsIURI> result_uri = nsNewURI(favicon_url);
        
        m_wnd->FetchFavIcon((void*)result_uri.p);
        return NS_OK;
    }
    
    if (type == wxT("keydown") ||
        type == wxT("keyup"))
    {
        int evtid;
        if (type == wxT("keydown"))
        {
            evtid = wxEVT_KEY_DOWN;
        }
         else if (type == wxT("keyup"))
        {
            evtid = wxEVT_KEY_UP;
        }
         else
        {
            wxFAIL_MSG(wxT("NS event type needs to be mapped to wxWebConnect event type"));
        }

        wxKeyEvent _wxKeyEvent(evtid);

        ns_smartptr<nsIDOMKeyEvent> key_evt = nsToSmart(evt);
        if (!key_evt)
            return NS_ERROR_NOT_IMPLEMENTED;

        PRBool altKey, shiftKey, ctrlKey, metaKey;
        PRUint32 keyCode;
        key_evt->GetAltKey(&altKey);
        key_evt->GetShiftKey(&shiftKey);
        key_evt->GetCtrlKey(&ctrlKey);
        key_evt->GetMetaKey(&metaKey);
        // XXX: Will this need some translating?
        key_evt->GetKeyCode(&keyCode);

        _wxKeyEvent.m_altDown = (bool)altKey;
        _wxKeyEvent.m_shiftDown = (bool)shiftKey;
        _wxKeyEvent.m_controlDown = (bool)ctrlKey;
        _wxKeyEvent.m_metaDown = (bool)metaKey;
        _wxKeyEvent.m_keyCode = (int)keyCode;

        // fill out and send a mouse event
        _wxKeyEvent.SetEventObject(m_wnd);

        m_wnd->GetEventHandler()->ProcessEvent(_wxKeyEvent);
    
        return NS_OK;
    }

    if (type == wxT("mousedown") ||
        type == wxT("mouseup") ||
        type == wxT("mouseover") ||
        type == wxT("mouseout") ||
        type == wxT("dblclick") ||
        type == wxT("dragdrop"))
    {
        if (type == wxT("mousedown"))
        {
            if (wxWindow::FindFocus() != m_wnd)
                m_wnd->SetFocus();
        }
        
        ns_smartptr<nsIDOMEventTarget> target;
        evt->GetTarget(&target.p);
        
        ns_smartptr<nsIDOMMouseEvent> mouse_evt = nsToSmart(evt);
        if (!mouse_evt)
            return NS_ERROR_NOT_IMPLEMENTED;
        
        int evtid;
        PRUint16 ns_button = 0;
        
        mouse_evt->GetButton(&ns_button);
        
        if (type == wxT("mousedown"))
        {
            switch (ns_button)
            {
                default:
                case 0: evtid = wxEVT_WEB_LEFTDOWN; break;
                case 1: evtid = wxEVT_WEB_MIDDLEDOWN; break;
                case 2: evtid = wxEVT_WEB_RIGHTDOWN; break;
            }
        }
         else if (type == wxT("mouseup"))
        {
            switch (ns_button)
            {
                default:
                case 0: evtid = wxEVT_WEB_LEFTUP; break;
                case 1: evtid = wxEVT_WEB_MIDDLEUP; break;
                case 2: evtid = wxEVT_WEB_RIGHTUP; break;
            }
        }
         else if (type == wxT("mouseover"))
        {
            evtid = wxEVT_WEB_MOUSEOVER;
        }
         else if (type == wxT("mouseout"))
        {
            evtid = wxEVT_WEB_MOUSEOUT;
        }
         else if (type == wxT("dblclick"))
        {
            evtid = wxEVT_WEB_LEFTDCLICK;
        }
         else if (type == wxT("dragdrop"))
        {
            evtid = wxEVT_WEB_DRAGDROP;
        }
         else
        {
            wxFAIL_MSG(wxT("NS event type needs to be mapped to wxWebConnect event type"));
        }
         
        
        // see if the target or any of its parent nodes are anchors and
        // if so, set the event href; note: the reason we have to look
        // through the parents of the target is because the target may
        // be a child node of the element where the actual href is specified,
        // and as a result, may not itself contain the href; this happens,
        // for example, when portions of the text in a hyperlink are bold
        ns_smartptr<nsIDOMNode> node;
        node = target;
        node.p = GetAnchor(node.p);
        
        ns_smartptr<nsIDOMHTMLAnchorElement> anchor;
        anchor = node;

        // fill out and send a mouse event
        wxWebEvent evt(evtid, m_wnd->GetId());
        evt.m_target_node.m_data->setNode(target);
        evt.SetEventObject(m_wnd);
        if (anchor)
        {
            nsEmbedString nss;
            anchor->GetHref(nss);
            evt.SetHref(ns2wx(nss));
            
            // also, we can handle this here:  if a link was clicked, clear out
            // our favicon stuff to prepare for the next load
            m_wnd->ResetFavicon();
            
        }
        m_wnd->GetEventHandler()->ProcessEvent(evt);
    
        return NS_OK;
    }

    return NS_ERROR_NOT_IMPLEMENTED;
}

wxWebControl* GetWebControlFromBrowserChrome(nsIWebBrowserChrome* chrome)
{
    if (!chrome)
        return NULL;

    ns_smartptr<nsIChromeInternal> chrome_int = ns_smartptr<nsIWebBrowserChrome>(chrome);
    
    if (chrome_int.empty())
        return NULL;

    return chrome_int->GetWebControl();
}




///////////////////////////////////////////////////////////////////////////////
//  ContentListener class implementation
///////////////////////////////////////////////////////////////////////////////


// ContentListener implements the ns interfaces for
// content listeners and patches them through to our
// public wxWebContentHandler class

class ContentListener : public nsIURIContentListener,
                        public nsIStreamListener,
                        public nsSupportsWeakReference
{
public:

    NS_DECL_ISUPPORTS

    ContentListener(wxWebContentHandler* handler)
    {
        NS_INIT_ISUPPORTS();
        
        m_handler = handler;
    }
    
    virtual ~ContentListener()
    {
    }
    
    NS_IMETHODIMP OnStartURIOpen(nsIURI* uri,
                                 PRBool* abort)
    {
        nsEmbedCString spec;
        nsresult res = uri->GetSpec(spec);
        if (NS_FAILED(res))
            return NS_OK;
      
        m_current_uri = ns2wx(spec);

        *abort = PR_FALSE;
        return NS_OK;
    }

    NS_IMETHODIMP DoContent(const char* content_type,
                            PRBool is_content_preferred,
                            nsIRequest* request,
                            nsIStreamListener** content_handler,
                            PRBool* retval)
    {
        *retval = PR_FALSE;
        *content_handler = static_cast<nsIStreamListener*>(this);
        (*content_handler)->AddRef();
        return NS_OK;
    }

    NS_IMETHODIMP IsPreferred(const char* content_type,
                              char** desired_content_type,
                              PRBool* retval)
    {
        return CanHandleContent(content_type, PR_TRUE, desired_content_type, retval);
    }

    NS_IMETHODIMP CanHandleContent(const char* _content_type,
                                   PRBool is_content_preferred,
                                   char** desired_content_type,
                                   PRBool* retval)
    {
        wxString content_type = wxString::FromAscii(_content_type);
        content_type.MakeLower();
        
        *retval = m_handler->CanHandleContent(m_current_uri, content_type) ? PR_TRUE : PR_FALSE;
        return NS_OK;
     }

    NS_IMETHODIMP GetLoadCookie(nsISupports** load_cookie)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP SetLoadCookie(nsISupports* load_cookie)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP GetParentContentListener(nsIURIContentListener** parent_content_listener)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP SetParentContentListener(nsIURIContentListener* parent_content_listener)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    // nsIStreamListener
    
    NS_IMETHODIMP OnStartRequest(nsIRequest* request, nsISupports* context)
    {
        ns_smartptr<nsIRequest> sp = request;
        ns_smartptr<nsIChannel> channel = sp;
        ns_smartptr<nsIURI> uri;
        channel->GetURI(&uri.p);
        
        nsEmbedCString spec;

        if (NS_FAILED(uri->GetSpec(spec)))
            return NS_OK;
            
        wxString url = ns2wx(spec);
        
        m_handler->OnStartRequest(url);
        return NS_OK;
    }
    
    NS_IMETHODIMP OnStopRequest(nsIRequest* request, nsISupports* context, nsresult status_code)
    {
        m_handler->OnStopRequest();
        return NS_OK;
    }
    
    NS_IMETHODIMP OnDataAvailable(nsIRequest* channel,
                                  nsISupports* context,
                                  nsIInputStream* in_stream,
                                  PRUint32 source_offset,
                                  PRUint32 count)
    {
        unsigned char* buf = new unsigned char[count];
        
        PRUint32 read;
        in_stream->Read((char*)buf, count, &read);
        
        m_handler->OnData(buf, count);
        
        delete[] buf;
        
        return NS_OK;
    }
    
private:

    wxWebContentHandler* m_handler;
    wxString m_current_uri;
};

NS_IMPL_ADDREF(ContentListener)
NS_IMPL_RELEASE(ContentListener)

NS_INTERFACE_MAP_BEGIN(ContentListener)
    NS_INTERFACE_MAP_ENTRY(nsIURIContentListener)
    NS_INTERFACE_MAP_ENTRY(nsIStreamListener)
    NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
NS_INTERFACE_MAP_END




///////////////////////////////////////////////////////////////////////////////
//  MainURIListener class implementation
///////////////////////////////////////////////////////////////////////////////


class MainURIListener : public nsIURIContentListener,
                        public nsSupportsWeakReference

{
    friend class wxWebControl;

public:

    NS_DECL_ISUPPORTS

    MainURIListener(wxWebControl* wnd, nsIWebBrowser* browser)
    {
        NS_INIT_ISUPPORTS();
        
        m_wnd = wnd;
        m_docshell_uri_listener = nsRequestInterface(browser);
    }
    
    virtual ~MainURIListener()
    {
    }
    
    NS_IMETHODIMP OnStartURIOpen(nsIURI* uri,
                                 PRBool* abort)
    {
        if (!m_wnd)
            return NS_OK;
        
        wxASSERT(uri);
        wxASSERT(abort);
        
        // set default behavior
        *abort = PR_FALSE;
        
        nsresult res;
        
        nsEmbedCString spec;
        res = uri->GetSpec(spec);
        if (NS_FAILED(res))
        {
            // should never happen, but allow the open if
            // something went wrong while getting the uri spec
            return NS_OK;
        }
        
        m_current_url = ns2wx(spec);
        
        wxWebEvent evt(wxEVT_WEB_OPENURI, m_wnd->GetId());
        evt.SetEventObject(m_wnd);
        evt.SetHref(m_current_url);
        if (m_wnd->GetEventHandler()->ProcessEvent(evt))
        {
            if (!evt.IsAllowed())
            {
                *abort = PR_TRUE;
                return NS_OK;
            }
        }
        
        
        // let all other content listeners know about this
        ContentListenerPtrArray& arr = g_gecko_engine.GetContentListeners();
        int i = 0, count = arr.GetCount();
        for (i = 0; i < count; ++i)
        {
            arr.Item(i)->OnStartURIOpen(uri, abort);
            if (*abort)
                return NS_OK;
        }
        
        
        return NS_OK;
    }

    NS_IMETHODIMP DoContent(const char* content_type,
                            PRBool is_content_preferred,
                            nsIRequest* request,
                            nsIStreamListener** content_handler,
                            PRBool* retval)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP IsPreferred(const char* content_type,
                              char** desired_content_type,
                              PRBool* retval)
    {
        return CanHandleContent(content_type, PR_TRUE, desired_content_type, retval);
    }

    NS_IMETHODIMP CanHandleContent(const char* _content_type,
                                   PRBool is_content_preferred,
                                   char** desired_content_type,
                                   PRBool* retval)
    {
        if (!m_wnd)
        {
            // no window, so return false (can't handle content)
            *retval = PR_FALSE;
            return NS_OK;
        }

        wxString content_type = wxString::FromAscii(_content_type);
        content_type.MakeLower();
        
        // this event will decide if the _browser_ should handle the content
        // or not.  If the browser doesn't handle the content, the content
        // listener(s) as specified by wxWebControl::AddContentHandler
        // are given a chance.
        
        // If a type conversion is specified, the browser will ask again
        // if the content can be handled
        
        wxWebEvent evt(wxEVT_WEB_SHOULDHANDLECONTENT, m_wnd->GetId());
        evt.SetEventObject(m_wnd);
        evt.SetContentType(content_type);
        evt.SetHref(m_current_url);
        if (m_wnd->GetEventHandler()->ProcessEvent(evt))
        {
            if (!evt.GetSkipped())
            {
                *retval = evt.m_should_handle ? PR_TRUE : PR_FALSE;
                wxString output_content_type = evt.m_output_content_type;
                output_content_type.MakeLower();
                if (output_content_type.Length() > 0 &&
                    output_content_type != content_type)
                {
                    *desired_content_type = (char*)NS_Alloc(output_content_type.Length()+1);
                    strcpy(*desired_content_type, (const char*)output_content_type.mbc_str());
                }
                
                return NS_OK;
            }
        }

        // default processing
        return m_docshell_uri_listener->CanHandleContent(_content_type,
                                                  is_content_preferred,
                                                  desired_content_type,
                                                  retval);
    }

    NS_IMETHODIMP GetLoadCookie(nsISupports** load_cookie)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP SetLoadCookie(nsISupports* load_cookie)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP GetParentContentListener(nsIURIContentListener** parent_content_listener)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

    NS_IMETHODIMP SetParentContentListener(nsIURIContentListener* parent_content_listener)
    {
        return NS_ERROR_NOT_IMPLEMENTED;
    }

private:

    wxWebControl* m_wnd;
    wxString m_current_url;
    ns_smartptr<nsIURIContentListener> m_docshell_uri_listener;
};


NS_IMPL_ADDREF(MainURIListener)
NS_IMPL_RELEASE(MainURIListener)

NS_INTERFACE_MAP_BEGIN(MainURIListener)
    NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIURIContentListener)
    NS_INTERFACE_MAP_ENTRY(nsIURIContentListener)
    NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
NS_INTERFACE_MAP_END




///////////////////////////////////////////////////////////////////////////////
//  WindowCreator implementation
///////////////////////////////////////////////////////////////////////////////


class WindowCreator : public nsIWindowCreator

{
public:

    NS_DECL_ISUPPORTS

    WindowCreator()
    {
        NS_INIT_ISUPPORTS();
    }
    
    virtual ~WindowCreator()
    {
    }
    
    NS_IMETHODIMP CreateChromeWindow(nsIWebBrowserChrome* parent,
                                     PRUint32 chrome_flags,
                                     nsIWebBrowserChrome** retval)
    {
        wxWebControl* web_control = GetWebControlFromBrowserChrome(parent);
        if (!web_control)
        {
            // no web control, so we can't create a new window
            // (this shouldn't happen)
            return NS_ERROR_FAILURE;
        }
        
        
        int wx_chrome_flags = 0;
        
        
        // TODO: add more flags as necessary
        if (chrome_flags & nsIWebBrowserChrome::CHROME_MODAL)
            wx_chrome_flags |= wxWEB_CHROME_MODAL;
        if (chrome_flags & nsIWebBrowserChrome::CHROME_WINDOW_RESIZE)
            wx_chrome_flags |= wxWEB_CHROME_RESIZABLE;
        if (chrome_flags & nsIWebBrowserChrome::CHROME_CENTER_SCREEN)
            wx_chrome_flags |= wxWEB_CHROME_CENTER;
        
        
        
        wxWebEvent evt(wxEVT_WEB_CREATEBROWSER, web_control->GetId());
        evt.SetEventObject(web_control);
        evt.SetCreateChromeFlags(wx_chrome_flags);
        web_control->GetEventHandler()->ProcessEvent(evt);

        if (!evt.IsAllowed())
        {
            // owner blocked creation of new browser
            // window with a veto
            return NS_ERROR_FAILURE;
        }
        
        if (evt.m_create_browser)
        {
            // owner supplied its own window
            *retval = static_cast<nsIWebBrowserChrome*>(evt.m_create_browser->m_chrome);
            NS_ADDREF(*retval);
            
            return NS_OK;
        }
         else
        {
            // owner did nothing, so we'll create our own window
            wxWebFrame* frame = new wxWebFrame(NULL, -1, wxT(""), wxPoint(50, 50), wxSize(550, 500));
            wxWebControl* ctrl = frame->GetWebControl();
            frame->Show(true);

            *retval = static_cast<nsIWebBrowserChrome*>(ctrl->m_chrome);
            NS_ADDREF(*retval);
        }
        
        return NS_OK;
    }
};


NS_IMPL_ISUPPORTS1(WindowCreator, nsIWindowCreator)




///////////////////////////////////////////////////////////////////////////////
//  PluginEnumerator implementation
///////////////////////////////////////////////////////////////////////////////


class PluginEnumerator : public nsISimpleEnumerator

{
public:

    NS_DECL_ISUPPORTS
    
    PluginEnumerator()
    {
        NS_INIT_ISUPPORTS();
        m_cur_item = 0;
    }
    
    virtual ~PluginEnumerator()
    {
    }
    
    NS_IMETHODIMP HasMoreElements(PRBool* retval)
    {
        if (!retval)
            return NS_ERROR_NULL_POINTER;
        
        if (m_cur_item >= m_paths.GetCount())
            *retval = PR_FALSE;
             else
            *retval = PR_TRUE;
            
        return NS_OK;
    }

    NS_IMETHODIMP GetNext(nsISupports** retval)
    {
        if (!retval)
            return NS_ERROR_NULL_POINTER;

        ns_smartptr<nsILocalFile> file;
        nsresult res = NS_NewNativeLocalFile(nsDependentCString((const char*)m_paths[m_cur_item].mbc_str()), PR_TRUE, &file.p);
        if (NS_FAILED(res))
            return NS_ERROR_NULL_POINTER;
            
        *retval = file.p;
        (*retval)->AddRef();
        
        ++m_cur_item;
        
        return NS_OK;
    }

    void SetPaths(const wxArrayString& paths)
    {
        m_paths = paths;
    }
    
private:

    wxArrayString m_paths;
    size_t m_cur_item;
    
};

NS_IMPL_ISUPPORTS1(PluginEnumerator, nsISimpleEnumerator)




///////////////////////////////////////////////////////////////////////////////
//  PluginListProvider implementation
///////////////////////////////////////////////////////////////////////////////


class PluginListProvider : public nsIDirectoryServiceProvider2

{
public:

    NS_DECL_ISUPPORTS

    PluginListProvider()
    {
        NS_INIT_ISUPPORTS();
    }
    
    virtual ~PluginListProvider()
    {
    }
    
    void AddPaths(nsISimpleEnumerator* paths)
    {
        PRBool more = PR_FALSE;
        
        while (1)
        {
            paths->HasMoreElements(&more);
            if (!more)
                break;
                
            nsISupports* element = NULL;
            paths->GetNext(&element);
            if (!element)
                continue;
                
            ns_smartptr<nsIFile> file = nsToSmart(element);
            element->Release();
            
            if (file)
            {
                nsEmbedCString path;
                file->GetNativePath(path);

                wxString wxpath = ns2wx(path);
                m_paths.Add(wxpath);
            }
        }
        
    }
    
    void AddPath(const wxString& path)
    {
        m_paths.Add(path);
    }

    NS_IMETHODIMP GetFile(const char* prop, PRBool* persistant, nsIFile** retval)
    {
        if (!retval)
            return NS_ERROR_NULL_POINTER;
            
        // nothing returned by this method;
        // let next directory provider handle the request
        return NS_ERROR_FAILURE;
    }

    NS_IMETHODIMP GetFiles(const char* prop, nsISimpleEnumerator** retval)
    {
        if (!retval)
            return NS_ERROR_NULL_POINTER;

        if (0 == strcmp(prop, "APluginsDL"))
        {
            PluginEnumerator* enumerator = new PluginEnumerator;
            enumerator->SetPaths(m_paths);
            *retval = enumerator;
            (*retval)->AddRef();
            return NS_OK;
        }
        
        // let next directory provider handle the request
        return NS_ERROR_FAILURE;
    }
  
private:

    wxArrayString m_paths;
};


NS_IMPL_ADDREF(PluginListProvider)
NS_IMPL_RELEASE(PluginListProvider)

NS_INTERFACE_MAP_BEGIN(PluginListProvider)
    NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIDirectoryServiceProvider)
    NS_INTERFACE_MAP_ENTRY(nsIDirectoryServiceProvider)
    NS_INTERFACE_MAP_ENTRY(nsIDirectoryServiceProvider2)
NS_INTERFACE_MAP_END




///////////////////////////////////////////////////////////////////////////////
//  GeckoEngine class implementation
///////////////////////////////////////////////////////////////////////////////


GeckoEngine::GeckoEngine()
{
    m_ok = false;
    m_is18 = false;
    m_plugin_provider = new PluginListProvider;
    m_plugin_provider->AddRef();
}

GeckoEngine::~GeckoEngine()
{
    m_plugin_provider->Release();
}

bool GeckoEngine::IsOk() const
{
    return m_ok;
}

void GeckoEngine::SetEnginePath(const wxString& path)
{
    wxASSERT_MSG(!m_ok, wxT("This must be called before the first wxWebControl is instantiated"));
    m_gecko_path = path;
}

void GeckoEngine::SetStoragePath(const wxString& path)
{
    wxASSERT_MSG(!m_ok, wxT("This must be called before the first wxWebControl is instantiated"));
    
    m_storage_path = path;
    
    wxChar path_separator = wxFileName::GetPathSeparator();
    m_history_filename = m_storage_path;
    if (m_history_filename.IsEmpty() || m_history_filename.Last() != path_separator)
        m_history_filename += path_separator;
    
    m_history_filename += wxT("kwkh01.dat");
}


// DelayedWindowDestroy is a utility class that will destroy
// a window a specified number of seconds later

class DelayedWindowDestroy : public wxTimer
{
public:
    DelayedWindowDestroy(wxWindow* wnd, int seconds)
    {
        m_wnd = wnd;
        Start(seconds*1000, wxTIMER_ONE_SHOT);
    }
    
    void Notify()
    {
        m_wnd->Destroy();
        
        if (!wxPendingDelete.Member(this))
            wxPendingDelete.Append(this);
  
    }
    
private:
    wxWindow* m_wnd;
};




// initialize the gecko engine; his is automatically called
// when wxWebControl objects are created.

bool GeckoEngine::Init()
{
    nsresult res;

    if (IsOk())
        return true;
    
    if (m_gecko_path.IsEmpty())
        return false;
    
    if (m_storage_path.IsEmpty())
    {
        wxLogNull log;

        wxString default_storage_path = wxStandardPaths::Get().GetTempDir();
        wxChar path_separator = wxFileName::GetPathSeparator();
        if (default_storage_path.IsEmpty() || default_storage_path.Last() != path_separator)
            default_storage_path += path_separator;
        default_storage_path += wxT("kwkh01.tmp");
        
#ifdef WIN32
        ::wxMkDir(default_storage_path);
#else
        ::wxMkDir((const char*)default_storage_path.mbc_str(), 0700);
#endif

        m_storage_path = default_storage_path;
    }

    SetStoragePath(m_storage_path);

    
    char path_separator = (char)wxFileName::GetPathSeparator();
    std::string gecko_path = (const char*)m_gecko_path.mbc_str();
    std::string xpcom_path = gecko_path;
    if (xpcom_path.empty() || xpcom_path[xpcom_path.length()-1] != path_separator)
        xpcom_path += path_separator;
    #if defined __WXMSW__
    xpcom_path += "xpcom.dll";
    #elif defined __WXMAC__
    xpcom_path += "libxpcom.dylib";
    #else
    xpcom_path += "libxpcom.so";
    #endif

        
    if (NS_FAILED(XPCOMGlueStartup(xpcom_path.c_str())))
        return false;
    
    ns_smartptr<nsILocalFile> gre_dir;
    res = NS_NewNativeLocalFile(nsDependentCString(gecko_path.c_str()), PR_TRUE, &gre_dir.p);
    if (NS_FAILED(res))
        return false;

    if (NS_FAILED(NS_InitXPCOM2(nsnull, gre_dir, nsnull)))
        return false;
    
    
    // create an app shell
    const nsCID appshell_cid = NS_APPSHELL_CID;
    m_appshell = nsCreateInstance(appshell_cid);
    if (m_appshell)
    {
        m_appshell->Create(0, nsnull);
        m_appshell->Spinup();
    }

    // set the window creator
    
    ns_smartptr<nsIWindowWatcher> window_watcher = nsGetWindowWatcherService();
    if (!window_watcher)
        return false;
        
    ns_smartptr<nsIWindowCreator> wnd_creator = static_cast<nsIWindowCreator*>(new WindowCreator);
    window_watcher->SetWindowCreator(wnd_creator);


    // set up our own custom prompting service
    
    ns_smartptr<nsIComponentRegistrar> comp_reg;
    res = NS_GetComponentRegistrar(&comp_reg.p);
    if (NS_FAILED(res))
        return false;
    
    ns_smartptr<nsIFactory> prompt_factory;
    CreatePromptServiceFactory(&prompt_factory.p);

    nsCID prompt_cid = NS_PROMPTSERVICE_CID;
    res = comp_reg->RegisterFactory(prompt_cid,
                                    "Prompt Service",
                                    "@mozilla.org/embedcomp/prompt-service;1",
                                    prompt_factory);

    prompt_factory.clear();
    CreatePromptServiceFactory(&prompt_factory.p);
    
    nsCID nssdialogs_cid = NS_NSSDIALOGS_CID;
    res = comp_reg->RegisterFactory(nssdialogs_cid,
                                    "PSM Dialog Impl",
                                    "@mozilla.org/nsBadCertListener;1",
                                    prompt_factory);

    // set up our own download progress service
    
    ns_smartptr<nsIFactory> transfer_factory;
    CreateTransferFactory(&transfer_factory.p);

    nsCID download_cid = NS_DOWNLOAD_CID;
    res = comp_reg->RegisterFactory(download_cid,
                                    "Transfer",
                                    "@mozilla.org/transfer;1",
                                    transfer_factory);
                                    
    res = comp_reg->RegisterFactory(download_cid,
                                    "Transfer",
                                    "@mozilla.org/download;1",
                                    transfer_factory);


    ns_smartptr<nsIFactory> unknowncontenttype_factory;
    CreateUnknownContentTypeHandlerFactory(&unknowncontenttype_factory.p);

    nsCID unknowncontenthtypehandler_cid = NS_UNKNOWNCONTENTTYPEHANDLER_CID;
    res = comp_reg->RegisterFactory(unknowncontenthtypehandler_cid,
                                    "Helper App Launcher Dialog",
                                    "@mozilla.org/helperapplauncherdialog;1",
                                    unknowncontenttype_factory);
                                    
/*
    // set up cert override service
    
    ns_smartptr<nsIFactory> certoverride_factory;
    CreateCertOverrideFactory(&certoverride_factory.p);
    
    nsCID certoverride_cid = NS_CERTOVERRIDE_CID;
    res = comp_reg->RegisterFactory(certoverride_cid,
                                    "PSM Cert Override Settings Service",
                                    "@mozilla.org/security/certoverride;1",
                                    certoverride_factory);
    */

    // set up some history file (which appears to be
    // required for downloads to work properly, even if we
    // don't store any history entries)

    ns_smartptr<nsIDirectoryService> dir_service = nsGetDirectoryService();
    ns_smartptr<nsIProperties> dir_service_props = dir_service;
    
    ns_smartptr<nsILocalFile> history_file;
    res = NS_NewNativeLocalFile(nsDependentCString((const char*)m_history_filename.mbc_str()), PR_TRUE, &history_file.p);
    if (NS_FAILED(res))
        return false;
        
    res = dir_service_props->Set("UHist", history_file.p);
    if (NS_FAILED(res))
        return false;
    
    // set up a profile directory, which is necessary for many
    // parts of the gecko engine, including ssl on linux

    ns_smartptr<nsILocalFile> prof_dir;
    res = NS_NewNativeLocalFile(nsDependentCString((const char*)m_storage_path.mbc_str()), PR_TRUE, &prof_dir.p);
    if (NS_FAILED(res))
        return false;

    res = dir_service_props->Set("ProfD", prof_dir.p);
    if (NS_FAILED(res))
        return false;
    
    
    // replace the old plugin directory enumerator with our own
    // but keep all the entries that were in there
    
    ns_smartptr<nsISimpleEnumerator> plugin_enum;
    res = dir_service_props->Get("APluginsDL", NS_GET_IID(nsISimpleEnumerator), (void**)&plugin_enum.p);
    if (NS_FAILED(res) || !plugin_enum.p)
        return false;
    
    m_plugin_provider->AddPaths(plugin_enum.p);
    res = dir_service->RegisterProvider(m_plugin_provider);
    if (NS_FAILED(res) || !plugin_enum.p)
        return false;
    

    // set up preferences
    
    ns_smartptr<nsIPref> prefs = nsGetPrefService();
    if (!prefs)
        return false;
    
    // this was originally so that we wouldn't have to set
    // up a prompting service.
    prefs->SetBoolPref("security.warn_submit_insecure", PR_FALSE);
    
    // don't store a history
    prefs->SetIntPref("browser.history_expire_days", 0);
    
    // set path for our cache directory
    PRUnichar* temps = wxToUnichar(m_storage_path);
    prefs->SetUnicharPref("browser.cache.disk.parent_directory", temps);
    freeUnichar(temps);


    m_ok = true;
    
    m_is18 = m_appshell.empty() ? false : true;
    
    
    if (m_is18)
    {
        // 24 May 2008 - a bug was discovered; if a web control is not created
        // in about 1 minute of the web engine being initialized, something goes
        // wrong with the message queue, and the web control will only update
        // when the mouse is moved over it-- strange.  I think there must be some
        // thread condition that waits until the first web control is created.
        // In any case, creating a web control here appears to solve the problem;
        // It's destroyed 10 seconds after creation.
        
        wxWebFrame* f = new wxWebFrame(NULL, -1, wxT(""));
        f->SetShouldPreventAppExit(false);
        f->GetWebControl()->OpenURI(wxT("about:blank"));
        
        DelayedWindowDestroy* d = new DelayedWindowDestroy(f, 10);
    }
    
    return true;
}

void GeckoEngine::AddContentListener(ContentListener* l)
{
    m_content_listeners.Add(l);
}

ContentListenerPtrArray& GeckoEngine::GetContentListeners()
{
    return m_content_listeners;
}

void GeckoEngine::AddPluginPath(const wxString& path)
{
    // check first if the path exists
    if (!wxFileName::DirExists(path))
        return;
        
    m_plugin_provider->AddPath(path);
}




///////////////////////////////////////////////////////////////////////////////
//  wxWebPostData class implementation
///////////////////////////////////////////////////////////////////////////////


void wxWebPostData::Add(const wxString& variable, const wxString& value)
{
    m_vars.Add(variable);
    m_values.Add(value);
}

static wxString urlEscape(const wxString& input)
{
    wxString result;
    result.Alloc(input.Length() + 10);
    
    const wxChar* ch = input.c_str();
    unsigned int c;
    
    wxString u = wxT(" ");

    while ((c = *ch))
    {
        if (c >= 128)
        {
            // we need to utf-8 encode this character per RFC-3986
            u.SetChar(0, *ch);
            const wxCharBuffer utf8b = u.utf8_str();
            const char* utf8 = (const char*)utf8b;
            while (*utf8)
            {
                result += wxString::Format(wxT("%%%02X"), (unsigned char)*utf8);
                ++utf8;
            }
            
            ch++;
            continue;
        }
        
        if (c <= 0x1f ||
            c == '%' || c == ' ' || c == '&' || c == '=' ||
            c == '+' || c == '$' || c == '#' || c == '{' ||
            c == '}' || c == '\\' ||c == '|' || c == '^' ||
            c == '~' || c == '[' || c == ']' || c == '`' ||
            c == '<' || c == '>')
        {
            result += wxString::Format(wxT("%%%02X"), c);
        }
         else
        {
            result += *ch;
        }
        
        ++ch;
    }

    return result;
}

wxString wxWebPostData::GetPostString()
{
    wxString result;
    wxString post_string;
    
    size_t i, cnt = m_vars.GetCount();
    
    for (i = 0; i < cnt; ++i)
    {
        if (i > 0)
            post_string += wxT("&");
        post_string += urlEscape(m_vars[i]);
        post_string += wxT("=");
        post_string += urlEscape(m_values[i]);
    }
    
    result = wxString::Format(wxT("Content-Length: %d\r\n"), post_string.Length());
    result += wxT("Content-Type: application/x-www-form-urlencoded\r\n\r\n");
    result += post_string;

    return result;
}

bool wxWebControl::InitEngine(const wxString& path)
{
    if (g_gecko_engine.IsOk())
    {
        wxFAIL_MSG(wxT("wxWebControl::InitEngine() should only be called once"));
    }

    g_gecko_engine.SetEnginePath(path);

    return g_gecko_engine.Init();
}

wxWebPreferences wxWebControl::GetPreferences()
{
    wxWebPreferences p;
    return p;
}

void wxWebControl::SetIgnoreCertErrors(bool ignore)
{
    g_ignore_ssl_cert_errors = ignore;
}

bool wxWebControl::GetIgnoreCertErrors()
{
    return g_ignore_ssl_cert_errors;
}

//static
bool wxWebControl::IsVersion18()
{
    return g_gecko_engine.IsVersion18();
}



///////////////////////////////////////////////////////////////////////////////
//  wxWebFavIconProgress class implementation
///////////////////////////////////////////////////////////////////////////////


class wxWebFavIconProgress : public wxWebProgressBase
{
public:

    wxWebFavIconProgress(wxWebControl* ctrl)
    {
        m_progress = NULL;
        m_ctrl = ctrl;
    }
    
    void SetFilename(const wxString& filename)
    {
        m_filename = filename;
    }
    
    ~wxWebFavIconProgress()
    {
        if (m_progress)
        {
            if (wxWebControl::IsVersion18())
                ((ProgressListenerAdaptor18*)m_progress)->ClearProgressReference();
                 else
                ((ProgressListenerAdaptor*)m_progress)->ClearProgressReference();
            
            m_progress->Release();
        }
    }

    void OnFinish()
    {
        m_ctrl->OnFavIconFetched(m_filename);
    }
    
    void SetProgressListener(nsIWebProgressListener* prog)
    {
        if (m_progress)
        {
            m_progress->Release();
        }
        
        m_progress = prog;
        
        if (m_progress)
        {
            m_progress->AddRef();
        }
    }
    
private:

    nsIWebProgressListener* m_progress;
    wxWebControl* m_ctrl;
    wxString m_filename;
};




///////////////////////////////////////////////////////////////////////////////
//  wxWebWaitUntilFinished class implementation
///////////////////////////////////////////////////////////////////////////////


class wxWebWaitUntilFinished : public wxWebProgressBase
{
public:

    wxWebWaitUntilFinished(bool* ptr)
    {
        m_ptr = ptr;
        *m_ptr = false;
    }
    
    void OnFinish()
    {
        *m_ptr = true;
    }
    
    void OnError(const wxString& message)
    {
        *m_ptr = true;
    }
    
private:
    bool* m_ptr;
};




///////////////////////////////////////////////////////////////////////////////
//  wxWebControl class implementation
///////////////////////////////////////////////////////////////////////////////


BEGIN_EVENT_TABLE(wxWebControl, wxControl)
    EVT_SIZE(wxWebControl::OnSize)
    EVT_SET_FOCUS(wxWebControl::OnSetFocus)
    EVT_KILL_FOCUS(wxWebControl::OnKillFocus)
END_EVENT_TABLE()

// (CONSTRUCTOR) wxWebControl::wxWebControl
// Description: Creates a new wxWebControl object.
//
// Syntax: wxWebControl::wxWebControl(wxWindow* parent,
//                                    wxWindowID id,
//                                    const wxPoint& pos,
//                                    const wxSize& size)
//
// Remarks: Creates a new wxWebControl object.

wxWebControl::wxWebControl(wxWindow* parent,
                           wxWindowID id,
                           const wxPoint& pos,
                           const wxSize& size)
                           : wxControl(parent, id, pos, size, wxNO_BORDER)
{
    // set return value for IsOk() to false until initialization can be
    // verified as successful (end of the constructor)
    m_ok = false;
    m_content_loaded = true;


    m_favicon_progress = NULL;

    m_ptrs = new EmbeddingPtrs;
    
    // create browser chrome
    BrowserChrome* chrome = new BrowserChrome(this);
    chrome->AddRef();
    m_chrome = chrome;

    // make sure gecko is initialized
    if (!g_gecko_engine.IsOk())
    {
        if (!g_gecko_engine.Init())
        {
            m_chrome->Release();
            m_chrome = NULL;
            return;
        }
    }

    nsresult res;

    // create gecko web browser component
    m_ptrs->m_web_browser = nsCreateInstance("@mozilla.org/embedding/browser/nsWebBrowser;1");
    if (!m_ptrs->m_web_browser)
    {
        wxASSERT(0);
        return;
    }

    chrome->m_web_browser = m_ptrs->m_web_browser;

    m_ptrs->m_base_window = m_ptrs->m_web_browser;
    if (!m_ptrs->m_base_window)
    {
        wxASSERT(0);
        return;
    }

    // create browser chrome
    m_ptrs->m_web_browser->SetContainerWindow(static_cast<nsIWebBrowserChrome*>(m_chrome));

    // set the type to contentWrapper
    ns_smartptr<nsIDocShellTreeItem> dsti = m_ptrs->m_web_browser;
    if (dsti)
    {
        dsti->SetItemType(nsIDocShellTreeItem::typeContentWrapper);
    }
     else
    {
        // 1.8.x support
        ns_smartptr<ns18IDocShellTreeItem> dsti = m_ptrs->m_web_browser;
        if (dsti.empty())
        {
            wxASSERT(0);
            return;
        }
        dsti->SetItemType(nsIDocShellTreeItem::typeContentWrapper);
    }
    
    // get base window interface and set its native window
    #ifdef __WXGTK__
    void* native_handle = (void*)m_wxwindow;
    #else
    void* native_handle = (void*)GetHandle();
    #endif

    wxSize cli_size = GetClientSize();
    res = m_ptrs->m_base_window->InitWindow(native_handle,
                                            nsnull,
                                            0, 0,
                                            cli_size.x, cli_size.y);
    if (NS_FAILED(res))
    {
        wxASSERT(0);
        return;
    }
      
    res = m_ptrs->m_base_window->Create();
    if (NS_FAILED(res))
    {
        wxASSERT(0);
        return;
    }
    
    // set our web progress listener

    nsIWeakReference* weak = NS_GetWeakReference((nsIWebProgressListener*)m_chrome);
    m_ptrs->m_web_browser->AddWebBrowserListener(weak, NS_GET_IID(nsIWebProgressListener));
    weak->Release();

    

    // set our URI content listener

    m_main_uri_listener = new MainURIListener(this, m_ptrs->m_web_browser);
    m_main_uri_listener->AddRef();
    m_ptrs->m_web_browser->SetParentURIContentListener(static_cast<nsIURIContentListener*>(m_main_uri_listener));

    // get the event target
    
    ns_smartptr<nsIDOMWindow> dom_window;
    m_ptrs->m_web_browser->GetContentDOMWindow(&dom_window.p);
    if (!dom_window)
    {
        wxASSERT(0);
        return;
    }
    
    
    ns_smartptr<nsIDOMWindow2> dom_window2(dom_window);
    if (dom_window2)
    {
        res = dom_window2->GetWindowRoot(&m_ptrs->m_event_target.p);
        if (NS_FAILED(res))
        {
            wxASSERT(0);
            return;
        }
    }
     else
    {
        // 1.8.x support
        ns_smartptr<ns18IDOMWindow2> dom_window2(dom_window);
        if (!dom_window2)
        {
            wxASSERT(0);
            return;
        }
        res = dom_window2->GetWindowRoot(&m_ptrs->m_event_target.p);
        if (NS_FAILED(res))
        {
            wxASSERT(0);
            return;
        }
    }


    // initialize chrome events
    m_chrome->ChromeInit();


    // get the nsIClipboardCommands interface
    m_ptrs->m_clipboard_commands = nsRequestInterface(m_ptrs->m_web_browser);
    if (!m_ptrs->m_clipboard_commands)
    {
        wxASSERT(0);
        return;
    }
    
    // get the nsIWebBrowserFind interface
    m_ptrs->m_web_browser_find = nsRequestInterface(m_ptrs->m_web_browser);
    if (!m_ptrs->m_web_browser_find)
    {
        wxASSERT(0);
        return;
    }
    
    // get the nsIWebNavigation interface
    m_ptrs->m_web_navigation = m_ptrs->m_web_browser;
    if (!m_ptrs->m_web_navigation)
    {
        wxASSERT(0);
        return;
    }
    
    m_favicon_progress = new wxWebFavIconProgress(this);

    // now that initialization is complete (and successful, tell IsOk()
    // to return true)
    m_ok = true;
    
    
    PRUnichar* ns_uri = wxToUnichar(L"about:blank");
    m_ptrs->m_web_navigation->LoadURI(ns_uri,
                                      nsIWebNavigation::LOAD_FLAGS_NONE,
                                      NULL,
                                      NULL,
                                      NULL);
    freeUnichar(ns_uri);
    
    // show the browser component
    res = m_ptrs->m_base_window->SetVisibility(PR_TRUE);
}

wxWebControl::~wxWebControl()
{
    m_main_uri_listener->m_wnd = NULL;
    m_main_uri_listener->Release();

    if (m_ok)
    {
        // destroy web browser
        m_ptrs->m_base_window->Destroy();
        m_ptrs->m_base_window.clear();
    
        // release chrome
        m_chrome->ChromeUninit();
        m_chrome->Release();
    }


    // delete any web content handlers that we 'own'
    size_t i, count;
    for (i = 0, count = m_to_delete.GetCount(); i < count; ++i)
    {
        wxWebContentHandler* handler = m_to_delete.Item(i);
        delete handler;
    }
    
    delete m_favicon_progress;
    delete m_ptrs;
}

// (METHOD) wxWebControl::IsOk
// Description:
//
// Syntax: bool wxWebControl::IsOk() const
//
// Remarks:
//
// Returns: Returns true if the wxWebControl is valid, and false otherwise.

bool wxWebControl::IsOk() const
{
    return m_ok;
}

// (METHOD) wxWebControl::Find
// Description:
//
// Syntax: bool wxWebControl::Find(const wxString& text, 
//                                 unsigned int flags)
//
// Remarks:
//
// Returns:

bool wxWebControl::Find(const wxString& text, 
                        unsigned int flags)
{
    if (m_ptrs->m_web_browser_find.empty())
        return false;

    PRUnichar* find_text = wxToUnichar(text);
    m_ptrs->m_web_browser_find->SetSearchString(find_text);
    freeUnichar(find_text);
        
    m_ptrs->m_web_browser_find->SetFindBackwards((flags & wxWEB_FIND_BACKWARDS) != 0 ? PR_TRUE : PR_FALSE);
    m_ptrs->m_web_browser_find->SetWrapFind((flags & wxWEB_FIND_WRAP) != 0 ? PR_TRUE : PR_FALSE);
    m_ptrs->m_web_browser_find->SetEntireWord((flags & wxWEB_FIND_ENTIRE_WORD) != 0 ? PR_TRUE : PR_FALSE);
    m_ptrs->m_web_browser_find->SetMatchCase((flags & wxWEB_FIND_MATCH_CASE) != 0 ? PR_TRUE : PR_FALSE);
    m_ptrs->m_web_browser_find->SetSearchFrames((flags & wxWEB_FIND_SEARCH_FRAMES) != 0 ? PR_TRUE : PR_FALSE);
    
    
    PRBool retval = PR_FALSE;
    m_ptrs->m_web_browser_find->FindNext(&retval);
    
    return retval ? true : false;
}

// (METHOD) wxWebControl::AddContentHandler
// Description:
//
// Syntax: bool wxWebControl::AddContentHandler(wxWebContentHandler* handler,
//                                              bool take_ownership)
//
// Remarks:
//
// Returns:

bool wxWebControl::AddContentHandler(wxWebContentHandler* handler,
                                     bool take_ownership)
{
    nsresult res;
    
    ns_smartptr<nsIServiceManager> service_mgr;
    res = NS_GetServiceManager(&service_mgr.p);
    if (NS_FAILED(res))
        return false;
    
    
    ns_smartptr<nsISupports> uri_loader;
    
    nsIID iid = NS_ISUPPORTS_IID;
    service_mgr->GetServiceByContractID("@mozilla.org/uriloader;1",
                                        iid,
                                        (void**)&uri_loader.p);
                                        
    if (uri_loader.empty())
        return false;

    ns_smartptr<nsIURILoader> uri_loader19 = uri_loader;
    if (uri_loader19)
    {
        ContentListener* l = new ContentListener(handler);
        l->AddRef(); // will be released later
        res = uri_loader19->RegisterContentListener(static_cast<nsIURIContentListener*>(l));
        if (NS_FAILED(res))
            return false;
        g_gecko_engine.AddContentListener(l);
    }
     else
    {
        ns_smartptr<ns18IURILoader> uri_loader18 = uri_loader;
        if (uri_loader18.empty())
            return false;
    
        ContentListener* l = new ContentListener(handler);
        l->AddRef(); // will be released later
        res = uri_loader18->RegisterContentListener(static_cast<nsIURIContentListener*>(l));
        if (NS_FAILED(res))
            return false;
        g_gecko_engine.AddContentListener(l);
    }
    
    return true;
}

// (METHOD) wxWebControl::AddPluginPath
// Description:
//
// Syntax: static void wxWebControl::AddPluginPath(const wxString& path)
//
// Remarks:
//
// Returns:

void wxWebControl::AddPluginPath(const wxString& path)
{
    g_gecko_engine.AddPluginPath(path);
}



// (METHOD) wxWebControl::SetProfilePath
// Description:
//
// Syntax: static void wxWebControl::SetProfilePath(const wxString& path)
//
// Remarks:
//
// Returns:

void wxWebControl::SetProfilePath(const wxString& path)
{
    g_gecko_engine.SetStoragePath(path);
}



// (METHOD) wxWebControl::SaveRequest
// Description:
//
// Syntax: static bool wxWebControl::SaveRequest(const wxString& uri_str,
//                                               const wxString& destination_path,
//                                               wxWebPostData* post_data,
//                                               wxWebProgressBase* listener)
//
// Remarks:
//
// Returns:

bool wxWebControl::SaveRequest(const wxString& uri_str,
                               const wxString& destination_path,
                               wxWebPostData* post_data,
                               wxWebProgressBase* listener)
{
    // make uri object out of the request uri
    ns_smartptr<nsIURI> uri = nsNewURI(uri_str);
    if (uri.empty())
        return false;
        
    ns_smartptr<nsIWebBrowserPersist> persist = nsCreateInstance("@mozilla.org/embedding/browser/nsWebBrowserPersist;1");
    if (!persist)
        return false;


    ns_smartptr<nsILocalFile> file = nsNewLocalFile(destination_path);
    if (file.empty())
    {
        wxFAIL_MSG(wxT("wxWebControl::SaveURI(): could not create temporary file"));
        return false;
    }




    // post data
    ns_smartptr<nsIInputStream> sp_post_data;
    if (post_data)
    {
        ns_smartptr<nsIStringInputStream> strs = nsCreateInstance("@mozilla.org/io/string-input-stream;1");
        wxASSERT(strs.p);
        
        if (strs)
        {
            wxString poststr = post_data->GetPostString();
            strs->SetData((const char*)poststr.mbc_str(), poststr.Length());
            sp_post_data = strs;
        }
    }
    

    nsresult rv;
    
    wxWebWaitUntilFinished* wuf = NULL;
    bool finished = false;
    
    if (listener)
    {
        // caller desires its own progress listener.  Caller is responsible
        // for maintaining a non-blocked main gui thread, otherwise
        // the callbacks will never be called.
        nsIWebProgressListener* la = CreateProgressListenerAdaptor(listener);
        persist->SetProgressListener(la);
        la->Release();
    }
     else
    {
        // caller wants to block until the request is finished
        wuf = new wxWebWaitUntilFinished(&finished);
        nsIWebProgressListener* la = CreateProgressListenerAdaptor(wuf);
        persist->SetProgressListener(la);
        la->Release();
    }
    
    persist->SetPersistFlags(nsIWebBrowserPersist::PERSIST_FLAGS_BYPASS_CACHE);
    
    rv = persist->SaveURI(uri, nsnull, nsnull, sp_post_data.p, nsnull, file);
    
    if (NS_FAILED(rv))
    {
        // free up progress listener
        persist->SetProgressListener(nsnull);
    }
     else
    {
        if (wuf)
        {
            wxStopWatch sw;
            wxWindowDisabler wd;
            
            while (!finished)
            {
                // gecko uses the main thread for status callbacks
                // so this wxSafeYield call is necessary
                
                if (wxThread::IsMain())
                {
                    ::wxWakeUpIdle();
                    ::wxYield();
                }

                wxThread::Sleep(10);
                
                // timeout, right now hardcoded
                if (sw.Time() > 30000)
                    break;
            }
        }
    }
    
    return NS_SUCCEEDED(rv) ? true : false;
}

// (METHOD) wxWebControl::SaveRequestToString
// Description:
//
// Syntax: static bool wxWebControl::SaveRequestToString(const wxString& uri_str,
//                                                       wxString* result,
//                                                       wxWebPostData* post_data,
//                                                       wxWebProgressBase* listener)
//
// Remarks:
//
// Returns:

bool wxWebControl::SaveRequestToString(const wxString& uri_str,
                                       wxString* result,
                                       wxWebPostData* post_data,
                                       wxWebProgressBase* listener)
{
    
    wxString filename = wxFileName::CreateTempFileName(wxT("wwc"));
    if (!SaveRequest(uri_str, filename, post_data, listener))
        return false;
    
    if (!result)
    {
        ::wxRemoveFile(filename);
        return true;
    }
        
    *result = wxT("");
    
    wxFile f;
    if (f.Open(filename) && result)
    {
        wxString res;
        
        char buf[1025];
        while (1)
        {
            size_t r = f.Read(buf, 1024);
            if (r == 0)
                break;
            buf[r] = 0;
            
            res += wxString::FromAscii(buf);
            
            if (r != 1024)
                break;
        }
        
        f.Close();
        
        *result = res;
    }
    
    ::wxRemoveFile(filename);
    return true;
}

// (METHOD) wxWebControl::ClearCache
// Description:
//
// Syntax: static bool wxWebControl::ClearCache()
//
// Remarks:
//
// Returns:

bool wxWebControl::ClearCache()
{
    ns_smartptr<nsICacheService> cache_service = nsGetService("@mozilla.org/network/cache-service;1");
    if (cache_service)
    {
        cache_service->EvictEntries(0 /*nsICache::STORE_ANYWHERE*/);
        cache_service->EvictEntries(1 /*nsICache::STORE_IN_MEMORY*/);
        cache_service->EvictEntries(2 /*nsICache::STORE_ON_DISK*/);
        cache_service->EvictEntries(3 /*nsICache::STORE_ON_DISK_AS_FILE*/);
        return true;
    }
     else
    {
        return false;
    }
}

void wxWebControl::FetchFavIcon(void* _uri)
{
    if (m_favicon_fetched)
        return;
    m_favicon_fetched = true;

    nsEmbedCString ns_spec;
    wxString spec;

    nsIURI* raw_uri = (nsIURI*)_uri;
    ns_smartptr<nsIURI> uri = raw_uri;
    uri->GetSpec(ns_spec);
    spec = ns2wx(ns_spec);
    
    ns_smartptr<nsIWebBrowserPersist> persist = nsCreateInstance("@mozilla.org/embedding/browser/nsWebBrowserPersist;1");
    if (!persist)
        return;


    wxString extension = spec.AfterLast(L'.');
    extension = extension.Left(4);
    if (extension.IsEmpty())
        extension = L"tmp";
    extension.MakeLower();
    
    wxString filename = wxFileName::CreateTempFileName(L"fav");
    filename += wxT(".");
    filename += extension;
    ns_smartptr<nsILocalFile> file = nsNewLocalFile(filename);
    

    m_favicon_progress->SetFilename(filename);
    nsIWebProgressListener* la = CreateProgressListenerAdaptor(m_favicon_progress);
    m_favicon_progress->SetProgressListener(la);
    persist->SetProgressListener(la);
    la->Release();

    
    nsresult rv = persist->SaveURI(uri, nsnull, nsnull, nsnull, nsnull, file);
    
    if (NS_FAILED(rv))
    {
        persist->SetProgressListener(nsnull);
        return;
    }
}

void wxWebControl::ResetFavicon()
{
    m_favicon = wxImage();
    m_favicon_fetched = false;
    m_content_loaded = false;
}
   
void wxWebControl::OnFavIconFetched(const wxString& filename)
{
    wxLogNull nulllog;
    
    m_favicon = wxImage();
    if (!m_favicon.LoadFile(filename))
        return;
    ::wxRemove(filename);
    
    wxWebEvent cevt(wxEVT_WEB_FAVICONAVAILABLE, GetId());
    cevt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(cevt);
}


void wxWebControl::OnDOMContentLoaded()
{
    m_content_loaded = true;
    
    // fire the DOMContentLoaded event
    wxWebEvent evt(wxEVT_WEB_DOMCONTENTLOADED, GetId());
    evt.SetEventObject(this);
    GetEventHandler()->ProcessEvent(evt);
}

// (METHOD) wxWebControl::GetFavIcon
// Description:
//
// Syntax: wxImage wxWebControl::GetFavIcon() const
//
// Remarks:
//
// Returns:

wxImage wxWebControl::GetFavIcon() const
{
    return m_favicon;
}

// (METHOD) wxWebControl::GetDOMDocument
// Description:
//
// Syntax: wxDOMDocument wxWebControl::GetDOMDocument()
//
// Remarks:
//
// Returns:

wxDOMDocument wxWebControl::GetDOMDocument()
{
    wxDOMDocument doc;
    
    
    ns_smartptr<nsIDOMWindow> dom_window;
    m_ptrs->m_web_browser->GetContentDOMWindow(&dom_window.p);
    if (!dom_window)
        return doc;
    
    ns_smartptr<nsIDOMDocument> dom_doc;
    dom_window->GetDocument(&dom_doc.p);
    doc.m_data->setNode(dom_doc);
    
    wxASSERT(doc.IsOk());
    
    return doc;
}

// (METHOD) wxWebControl::OpenURI
// Description:
//
// Syntax: void wxWebControl::OpenURI(const wxString& uri,
//                                    unsigned int load_flags,
//                                    wxWebPostData* post_data)
//
// Remarks:
//
// Returns:

void wxWebControl::OpenURI(const wxString& uri,
                           unsigned int load_flags,
                           wxWebPostData* post_data)
{
    if (!IsOk())
        return;

    m_favicon = wxImage();
    m_favicon_fetched = false;
    m_content_loaded = false;
    
    unsigned int ns_load_flags = nsIWebNavigation::LOAD_FLAGS_NONE;
    
    if (load_flags & wxWEB_LOAD_LINKCLICK)
        ns_load_flags |= nsIWebNavigation::LOAD_FLAGS_IS_LINK;
        
    // post data
    ns_smartptr<nsIInputStream> sp_post_data;
    if (post_data)
    {
        ns_smartptr<nsIStringInputStream> strs = nsCreateInstance("@mozilla.org/io/string-input-stream;1");
        wxASSERT(strs.p);
        
        if (strs)
        {
            wxString poststr = post_data->GetPostString();
            strs->SetData((const char*)poststr.mbc_str(), poststr.Length());
            sp_post_data = strs;
        }
    }
       
    
    
    PRUnichar* ns_uri = wxToUnichar(uri);

    nsresult res;
    res = m_ptrs->m_web_navigation->LoadURI(ns_uri,
                                            ns_load_flags,
                                            NULL,
                                            sp_post_data.p,
                                            NULL);

    freeUnichar(ns_uri);
                                            
    ns_smartptr<nsIWebBrowserFocus> focus = nsRequestInterface(m_ptrs->m_web_browser);
    if (!focus)
        return;

    focus->Activate();
}

// (METHOD) wxWebControl::GetCurrentURI
// Description:
//
// Syntax: wxString wxWebControl::GetCurrentURI() const
//
// Remarks:
//
// Returns:

wxString wxWebControl::GetCurrentURI() const
{
    ns_smartptr<nsIURI> uri;
    
    m_ptrs->m_web_navigation->GetCurrentURI(&uri.p);
    
    if (uri.empty())
        return wxEmptyString;
    
    nsEmbedCString spec;

    if (NS_FAILED(uri->GetSpec(spec)))
        return wxEmptyString;
        
    return ns2wx(spec);
}

bool wxWebControl::SetContent(const wxString& strBaseURI, const wxString& strContent, const wxString& strContentType)
{
    m_content_loaded = false;

    ns_smartptr<nsIURI> uri;
    if (!strBaseURI.IsEmpty())
        uri = nsNewURI(strBaseURI);

    nsCString nsContentType;
    wx2ns(strContentType, nsContentType);

    std::string strutf8data = std::string(strContent.ToUTF8() );	//mod by glman

    ns_smartptr<nsIWebBrowserStream> browserStream(m_ptrs->m_web_browser);
    if (!browserStream.p)   {
        return false;
    }

    browserStream->OpenStream(uri, nsContentType);
    browserStream->AppendToStream((const PRUint8*)strutf8data.c_str(), strutf8data.length());
    browserStream->CloseStream();

    return true;
/*
    nsresult rv;
    m_content_loaded = false;

    ns_smartptr<nsIStringInputStream> inputStream(nsCreateInstance("@mozilla.org/io/string-input-stream;1"));
    fprintf(stderr, "Input stream created\n");
    if (!inputStream.p)
        return false;
    // convert our string to stream
    std::string strutf8data = strContent.ToUTF8();
    inputStream->SetData( strutf8data.c_str(), strutf8data.length() );
    fprintf(stderr, "Data set on input stream\n");
    // get document shell 
    //ns_smartptr<nsIScriptGlobalObject> sgo(m_ptrs->m_web_browser);
    //ns_smartptr<nsIScriptGlobalObject> sgo = nsRequestInterface(m_ptrs->m_web_browser);
    if (sgo.empty())
        fprintf(stderr, "No nsIScriptGlobalObject\n");
        return false;
    ns_smartptr<nsIDocShell> docShell(sgo->GetDocShell());
    //ns<nsPIDOMWindow> window = do_QueryInterface(aContext);
    ns_smartptr<nsIDocShell> docShell = nsRequestInterface(m_ptrs->m_web_browser);
    if (!docShell.p)
        fprintf(stderr, "Not got doc shell\n");
        return false;
    ns_smartptr<nsIURI> uri;
    if (!strBaseURI.IsEmpty())
        uri = nsNewURI(strBaseURI);
    fprintf(stderr, "strContentType: %s\n", strContentType.ToAscii());
    nsCString nsContentType;
    wx2ns(strContentType, nsContentType);
    // finally, load the stream we made!
    rv = docShell->LoadStream( inputStream, uri.p, nsContentType, nsDependentCString("UTF-8"), NULL);
    fprintf(stderr, "docShell LoadStream.\n");

    if (NS_FAILED(rv))
        fprintf(stderr, "Load stream failed\n");
        return false; 
    fprintf(stderr, "ending WebControl::SetContent()\n");
    return true;
*/
}

// (METHOD) wxWebControl::GoForward
// Description:
//
// Syntax: void wxWebControl::GoForward()
//
// Remarks:
//
// Returns:

void wxWebControl::GoForward()
{
    if (!IsOk())
        return;

    m_ptrs->m_web_navigation->GoForward();
}

// (METHOD) wxWebControl::GoBack
// Description:
//
// Syntax: void wxWebControl::GoBack()
//
// Remarks:
//
// Returns:

void wxWebControl::GoBack()
{
    if (!IsOk())
        return;

    m_ptrs->m_web_navigation->GoBack();
}

// (METHOD) wxWebControl::Reload
// Description:
//
// Syntax: void wxWebControl::Reload()
//
// Remarks:
//
// Returns:

void wxWebControl::Reload()
{
    if (!IsOk())
        return;

    m_ptrs->m_web_navigation->Reload(nsIWebNavigation::LOAD_FLAGS_NONE);
}

// (METHOD) wxWebControl::Stop
// Description:
//
// Syntax: void wxWebControl::Stop()
//
// Remarks:
//
// Returns:

void wxWebControl::Stop()
{
    if (!IsOk())
        return;

    m_ptrs->m_web_navigation->Stop(nsIWebNavigation::STOP_ALL);
}

// (METHOD) wxWebControl::IsContentLoaded
// Description:
//
// Syntax: bool wxWebControl::IsContentLoaded() const
//
// Remarks:
//
// Returns:

bool wxWebControl::IsContentLoaded() const
{
    return m_content_loaded;
}

wxString wxWebControl::GetCurrentLoadURI()
{
    return m_main_uri_listener->m_current_url;
}

void wxWebControl::InitPrintSettings()
{
    if (m_ptrs->m_print_settings.empty())
    {
        ns_smartptr<nsIPrintSettingsService> print_settings_service;
        print_settings_service = nsGetService("@mozilla.org/gfx/printsettings-service;1");
        if (print_settings_service)
        {
            ns_smartptr<nsIPrintSettings> print_settings;
            
            print_settings_service->GetGlobalPrintSettings(&print_settings.p);

            PRUnichar* printer_name = NULL;
            print_settings_service->GetDefaultPrinterName(&printer_name);
            if (printer_name)
                print_settings_service->InitPrintSettingsFromPrinter(printer_name, print_settings);
                
            print_settings_service->InitPrintSettingsFromPrefs(print_settings, 
                                                             PR_TRUE, 
                                                             nsIPrintSettings::kInitSaveAll);
                                                             
            m_ptrs->m_print_settings = print_settings;
        }
         else
        {
            ns_smartptr<nsIWebBrowserPrint> web_browser_print = nsRequestInterface(m_ptrs->m_web_browser);
            if (!web_browser_print)
            {
                wxASSERT(0);
                return;
            }

            ns_smartptr<nsISupports> supports;
            web_browser_print->GetGlobalPrintSettings((nsIPrintSettings**)&supports.p);
            m_ptrs->m_print_settings = supports;
        }
    }
}

// (METHOD) wxWebControl::Print
// Description:
//
// Syntax: void wxWebControl::Print(bool silent)
//
// Remarks:
//
// Returns:

void wxWebControl::Print(bool silent)
{
    // get the nsIWebBrowserPrint interface
    ns_smartptr<nsIWebBrowserPrint> web_browser_print = nsRequestInterface(m_ptrs->m_web_browser);
    if (!web_browser_print)
    {
        wxASSERT(0);
        return;
    }
    
    InitPrintSettings();
    
    ns_smartptr<nsIPrintSettings18> settings18 = m_ptrs->m_print_settings;
    if (settings18)
    {
        settings18->SetShowPrintProgress(PR_FALSE);
        settings18->SetPrintSilent(silent ? PR_TRUE : PR_FALSE);
        
        ns_smartptr<nsIWebBrowserPrint18> web_browser_print = nsRequestInterface(m_ptrs->m_web_browser);
        web_browser_print->Print(settings18.p, NULL);
    }

    ns_smartptr<nsIPrintSettings> settings19 = m_ptrs->m_print_settings;
    if (settings19)
    {
        settings19->SetShowPrintProgress(PR_FALSE);
        settings19->SetPrintSilent(silent ? PR_TRUE : PR_FALSE);
        web_browser_print->Print(settings19, NULL);
    }
    
}

// (METHOD) wxWebControl::SetPageSettings
// Description:
//
// Syntax: void wxWebControl::SetPageSettings(double page_width, double page_height,
//                                            double left_margin, double right_margin, 
//                                            double top_margin, double bottom_margin)
//
// Remarks:
//
// Returns:

void wxWebControl::SetPageSettings(double page_width, double page_height,
                                   double left_margin, double right_margin, 
                                   double top_margin, double bottom_margin)
{
    // get the nsIWebBrowserPrint interface
    ns_smartptr<nsIWebBrowserPrint> web_browser_print = nsRequestInterface(m_ptrs->m_web_browser);
    if (!web_browser_print)
    {
        wxASSERT(0);
        return;
    }

    
    InitPrintSettings();
    
    
    ns_smartptr<nsIPrintSettings18> settings18 = m_ptrs->m_print_settings;
    if (settings18)
    {
        // if the page width is greater than the page height,
        // set the proper orientation
        settings18->SetOrientation(settings18->kPortraitOrientation);
        if (page_width > page_height)
        {
            double t = page_width;
            page_width = page_height;
            page_height = t;

            settings18->SetOrientation(settings18->kLandscapeOrientation);
        }

        settings18->SetPaperWidth(page_width);
        settings18->SetPaperHeight(page_height);
        settings18->SetMarginLeft(left_margin);
        settings18->SetMarginRight(right_margin);
        settings18->SetMarginTop(top_margin);
        settings18->SetMarginBottom(bottom_margin);
    }
    
    
    ns_smartptr<nsIPrintSettings> settings19 = m_ptrs->m_print_settings;
    if (settings19)
    {
        // if the page width is greater than the page height,
        // set the proper orientation
        settings19->SetOrientation(settings19->kPortraitOrientation);
        if (page_width > page_height)
        {
            double t = page_width;
            page_width = page_height;
            page_height = t;

            settings19->SetOrientation(settings19->kLandscapeOrientation);
        }

        settings19->SetPaperWidth(page_width);
        settings19->SetPaperHeight(page_height);
        settings19->SetMarginLeft(left_margin);
        settings19->SetMarginRight(right_margin);
        settings19->SetMarginTop(top_margin);
        settings19->SetMarginBottom(bottom_margin);
    }
}

// (METHOD) wxWebControl::GetPageSettings
// Description:
//
// Syntax: void wxWebControl::GetPageSettings(double* page_width, double* page_height,
//                                            double* left_margin, double* right_margin, 
//                                            double* top_margin, double* bottom_margin)
//
// Remarks:
//
// Returns:

void wxWebControl::GetPageSettings(double* page_width, double* page_height,
                                   double* left_margin, double* right_margin, 
                                   double* top_margin, double* bottom_margin)
{
    // get the nsIWebBrowserPrint interface
    ns_smartptr<nsIWebBrowserPrint> web_browser_print = nsRequestInterface(m_ptrs->m_web_browser);
    if (!web_browser_print)
    {
        wxASSERT(0);
        return;
    }


    InitPrintSettings();


    ns_smartptr<nsIPrintSettings18> settings18 = m_ptrs->m_print_settings;
    if (settings18)
    {
        settings18->GetPaperWidth(page_width);
        settings18->GetPaperHeight(page_height);
        settings18->GetMarginLeft(left_margin);
        settings18->GetMarginRight(right_margin);
        settings18->GetMarginTop(top_margin);
        settings18->GetMarginBottom(bottom_margin);
        
        // if the orientation is set, reverse the page width
        // and page height
        PRInt32 orientation;
        settings18->GetOrientation(&orientation);
        if (orientation == settings18->kLandscapeOrientation)
        {
            double t = *page_width;
            *page_width = *page_height;
            *page_height = t;
        }
    }
    
    ns_smartptr<nsIPrintSettings> settings19 = m_ptrs->m_print_settings;
    if (settings19)
    {
        settings19->GetPaperWidth(page_width);
        settings19->GetPaperHeight(page_height);
        settings19->GetMarginLeft(left_margin);
        settings19->GetMarginRight(right_margin);
        settings19->GetMarginTop(top_margin);
        settings19->GetMarginBottom(bottom_margin);
        
        // if the orientation is set, reverse the page width
        // and page height
        PRInt32 orientation;
        settings19->GetOrientation(&orientation);
        if (orientation == settings19->kLandscapeOrientation)
        {
            double t = *page_width;
            *page_width = *page_height;
            *page_height = t;
        }
    }
}

// (METHOD) wxWebControl::ViewSource
// Description:
//
// Syntax: void wxWebControl::ViewSource()
// Syntax: void wxWebControl::ViewSource(wxWebControl* source_web_browser)
// Syntax: void wxWebControl::ViewSource(const wxString& uri)
//
// Remarks:
//
// Returns:

void wxWebControl::ViewSource()
{
    ViewSource(this);
}

void wxWebControl::ViewSource(wxWebControl* source_web_browser)
{
    ViewSource(source_web_browser->GetCurrentURI());

/*
    // this code is supposed to download the page from the cache,
    // but it's not working right now.  nsIWebBrowserPersist::SaveURI
    // isn't finished saving the document by the time that OpenURI is called
    
    ns_smartptr<nsIWebBrowserPersist> persist = source_web_browser->m_ptrs->m_web_browser;
    
    if (persist.empty())
    {
        wxFAIL_MSG(wxT("wxWebControl::ViewSource(): nsIWebBrowserPersist interface not available"));
        return;
    }
        
    wxString filename = wxFileName::CreateTempFileName(wxT("wwc"));
    wxString new_filename = filename + wxT(".html");
    ::wxRenameFile(filename, new_filename);
    filename = new_filename;
    
    ns_smartptr<nsILocalFile> file = nsNewLocalFile(filename);
    
    if (file.empty())
    {
        wxFAIL_MSG(wxT("wxWebControl::ViewSource(): could not create temporary file"));
        return;
    }

    nsresult res = persist->SaveURI(nsnull, nsnull, nsnull, nsnull, nsnull, file.p);
    
    wxString view_url = wxT("view-source:file:///");
    filename.Replace(wxT("\\"), wxT("/"));
    view_url += filename;
    
    OpenURI(view_url);
*/
}

void wxWebControl::ViewSource(const wxString& uri)
{
    wxString loc = wxT("view-source:");
    loc += uri;
    OpenURI(loc);
}

// (METHOD) wxWebControl::SaveCurrent
// Description:
//
// Syntax: bool wxWebControl::SaveCurrent(const wxString& destination_path)
//
// Remarks:
//
// Returns:

bool wxWebControl::SaveCurrent(const wxString& destination_path)
{
    // this code is supposed to download the page from the cache,
    // but it's not working right now.  nsIWebBrowserPersist::SaveURI
    // isn't finished saving the document by the time that OpenURI is called

    ns_smartptr<nsIWebBrowserPersist> persist = m_ptrs->m_web_browser;
    
    if (persist.empty())
    {
        wxFAIL_MSG(wxT("wxWebControl::ViewSource(): nsIWebBrowserPersist interface not available"));
        return false;
    }
        
    
    ns_smartptr<nsILocalFile> file = nsNewLocalFile(destination_path);
    
    if (file.empty())
    {
        wxFAIL_MSG(wxT("wxWebControl::ViewSource(): could not create temporary file"));
        return false;
    }

    nsresult res = persist->SaveURI(nsnull, // url - nsnull equals "this document"
                                    nsnull, // cache key
                                    nsnull, // nsIURI referrer
                                    nsnull, // post data
                                    nsnull, // extra headers
                                    file.p); // target file
    
    if (NS_FAILED(res))
        return false;
        
    return true;
}

// (METHOD) wxWebControl::GetTextZoom
// Description:
//
// Syntax: void wxWebControl::GetTextZoom(float* zoom)
//
// Remarks:
//
// Returns:

void wxWebControl::GetTextZoom(float* zoom)
{
    if (!IsOk())
        return;

    ns_smartptr<nsIDOMWindow> dom_window;
    m_ptrs->m_web_browser->GetContentDOMWindow(&dom_window.p);
    dom_window->GetTextZoom(zoom);
}

// (METHOD) wxWebControl::SetTextZoom
// Description:
//
// Syntax: void wxWebControl::SetTextZoom(float zoom)
//
// Remarks:
//
// Returns:

void wxWebControl::SetTextZoom(float zoom)
{
    if (!IsOk())
        return;

    ns_smartptr<nsIDOMWindow> dom_window;
    m_ptrs->m_web_browser->GetContentDOMWindow(&dom_window.p);
    dom_window->SetTextZoom(zoom);
}

// (METHOD) wxWebControl::CanCutSelection
// Description:
//
// Syntax: bool wxWebControl::CanCutSelection()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanCutSelection()
{
    if (!IsOk())
        return false;
    
    PRBool retval;
    m_ptrs->m_clipboard_commands->CanCutSelection(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CanCopySelection
// Description:
//
// Syntax: bool wxWebControl::CanCopySelection()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanCopySelection()
{
    if (!IsOk())
        return false;

    PRBool retval;
    m_ptrs->m_clipboard_commands->CanCopySelection(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CanCopyLinkLocation
// Description:
//
// Syntax: bool wxWebControl::CanCopyLinkLocation()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanCopyLinkLocation()
{
    if (!IsOk())
        return false;

    PRBool retval;
    m_ptrs->m_clipboard_commands->CanCopyLinkLocation(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CanCopyImageLocation
// Description:
//
// Syntax: bool wxWebControl::CanCopyImageLocation()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanCopyImageLocation()
{
    if (!IsOk())
        return false;

    PRBool retval;
    m_ptrs->m_clipboard_commands->CanCopyImageLocation(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CanCopyImageContents
// Description:
//
// Syntax: bool wxWebControl::CanCopyImageContents()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanCopyImageContents()
{
    if (!IsOk())
        return false;

    PRBool retval;
    m_ptrs->m_clipboard_commands->CanCopyImageContents(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CanPaste
// Description:
//
// Syntax: bool wxWebControl::CanPaste()
//
// Remarks:
//
// Returns:

bool wxWebControl::CanPaste()
{
    if (!IsOk())
        return false;

    PRBool retval;
    m_ptrs->m_clipboard_commands->CanPaste(&retval);
    return retval ? true : false;
}

// (METHOD) wxWebControl::CutSelection
// Description:
//
// Syntax: void wxWebControl::CutSelection()
//
// Remarks:
//
// Returns:

void wxWebControl::CutSelection()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->CutSelection();
}

// (METHOD) wxWebControl::CopySelection
// Description:
//
// Syntax: void wxWebControl::CopySelection()
//
// Remarks:
//
// Returns:

void wxWebControl::CopySelection()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->CopySelection();
}

// (METHOD) wxWebControl::CopyLinkLocation
// Description:
//
// Syntax: void wxWebControl::CopyLinkLocation()
//
// Remarks:
//
// Returns:

void wxWebControl::CopyLinkLocation()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->CopyLinkLocation();
}

// (METHOD) wxWebControl::CopyImageLocation
// Description:
//
// Syntax: void wxWebControl::CopyImageLocation()
//
// Remarks:
//
// Returns:

void wxWebControl::CopyImageLocation()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->CopyImageLocation();
}

// (METHOD) wxWebControl::CopyImageContents
// Description:
//
// Syntax: void wxWebControl::CopyImageContents()
//
// Remarks:
//
// Returns:

void wxWebControl::CopyImageContents()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->CopyImageContents();
}

// (METHOD) wxWebControl::Paste
// Description:
//
// Syntax: void wxWebControl::Paste()
//
// Remarks:
//
// Returns:

void wxWebControl::Paste()
{
    if (!IsOk())
        return;
    
    m_ptrs->m_clipboard_commands->Paste();
}

// (METHOD) wxWebControl::SelectAll
// Description:
//
// Syntax: void wxWebControl::SelectAll()
//
// Remarks:
//
// Returns:

void wxWebControl::SelectAll()
{
    if (!IsOk())
        return;
        
    m_ptrs->m_clipboard_commands->SelectAll();
}

// (METHOD) wxWebControl::SelectNone
// Description:
//
// Syntax: void wxWebControl::SelectNone()
//
// Remarks:
//
// Returns:

void wxWebControl::SelectNone()
{
    if (!IsOk())
        return;
        
    m_ptrs->m_clipboard_commands->SelectNone();
}

void wxWebControl::OnSetFocus(wxFocusEvent& evt)
{
    if (!IsOk())
        return;

    ns_smartptr<nsIWebBrowserFocus> focus = nsRequestInterface(m_ptrs->m_web_browser);
    if (!focus)
        return;
    
    focus->Activate();
}

void wxWebControl::OnKillFocus(wxFocusEvent& evt)
{
}

void wxWebControl::OnSize(wxSizeEvent& evt)
{
    if (!IsOk())
        return;

    wxRect cli_rect = this->GetClientRect();

    if (m_ptrs->m_base_window)
    {
        m_ptrs->m_base_window->SetPositionAndSize(0, 0,
                                    cli_rect.GetWidth(),
                                    cli_rect.GetHeight(),
                                    PR_TRUE);
    }
}









