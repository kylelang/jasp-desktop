//
// Copyright (C) 2013-2018 University of Amsterdam
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program.  If not, see
// <http://www.gnu.org/licenses/>.
//

#include "analysisform.h"
#include "knownissues.h"
#include "boundcontrols/boundcontrol.h"
#include "utilities/qutils.h"
#include "controls/jasplistcontrol.h"
#include "controls/expanderbuttonbase.h"
#include "log.h"
#include "controls/jaspcontrol.h"
#include "rsyntax/rsyntax.h"

#include <QQmlProperty>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include "preferencesmodelbase.h"

using namespace std;

const QString AnalysisForm::rSyntaxControlName = "__RSyntaxTextArea";

AnalysisForm::AnalysisForm(QQuickItem *parent) : QQuickItem(parent)
{
	setObjectName("AnalysisForm");

	_rSyntax = new RSyntax(this);
	// _startRSyntaxTimer is used to call setRSyntaxText only once in a event loop.
	connect(this,									&AnalysisForm::infoChanged,					this, &AnalysisForm::helpMDChanged			);
	connect(this,									&AnalysisForm::infoBottomChanged,			this, &AnalysisForm::helpMDChanged			);
	connect(this,									&AnalysisForm::formCompletedSignal,			this, &AnalysisForm::formCompletedHandler,	Qt::QueuedConnection);
	connect(this,									&AnalysisForm::analysisChanged,				this, &AnalysisForm::knownIssuesUpdated,	Qt::QueuedConnection);
	connect(KnownIssues::issues(),					&KnownIssues::knownIssuesUpdated,			this, &AnalysisForm::knownIssuesUpdated,	Qt::QueuedConnection);
	connect(this,									&AnalysisForm::showAllROptionsChanged,		this, &AnalysisForm::setRSyntaxText,		Qt::QueuedConnection);
	connect(PreferencesModelBase::preferences(),	&PreferencesModelBase::showRSyntaxChanged,	this, &AnalysisForm::setRSyntaxText,		Qt::QueuedConnection);
	connect(PreferencesModelBase::preferences(),	&PreferencesModelBase::showAllROptionsChanged,	this, &AnalysisForm::showAllROptionsChanged, Qt::QueuedConnection	);
	connect(this,									&AnalysisForm::analysisChanged,				this, &AnalysisForm::setRSyntaxText,		Qt::QueuedConnection);
}

AnalysisForm::~AnalysisForm()
{
	Log::log() << "~AnalysisForm " << this << std::endl;
}

void AnalysisForm::runRScript(const QString & script, const QString & controlName, bool whiteListedVersion)
{
	if(_analysis && !_removed)
	{
		if(_valueChangedSignalsBlocked == 0)	_analysis->sendRScript(script, controlName, whiteListedVersion);
		else									_waitingRScripts.push(std::make_tuple(script, controlName, whiteListedVersion));
	}
}

void AnalysisForm::runFilter(const QString & name)
{
	if(_analysis && !_removed)
	{
		if(_valueChangedSignalsBlocked == 0)	_analysis->sendFilter(name);
		else									_waitingFilters.insert(name);
	}
}

void AnalysisForm::refreshAnalysis()
{
	_analysis->refresh();
}

QString AnalysisForm::generateWrapper() const
{
	return _rSyntax->generateWrapper();
}

void AnalysisForm::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &value)
{
	if (change == ItemChange::ItemSceneChange && !value.window)
		cleanUpForm();
	QQuickItem::itemChange(change, value);
}

void AnalysisForm::cleanUpForm()
{
	if (!_removed)
	{
		_removed = true;
		for (JASPControl* control : _dependsOrderedCtrls)
			// controls will be automatically deleted by the deletion of AnalysisForm
			// But they must be first disconnected: sometimes an event seems to be triggered before the item is completely destroyed
			control->cleanUp();

		_formCompleted = false;
	}
}

void AnalysisForm::runScriptRequestDone(const QString& result, const QString& controlName, bool hasError)
{
	if(_removed)
		return;

	if (controlName == rSyntaxControlName)
	{
		JASPControl* rSyntaxControl = getControl(controlName);
		if (hasError)
			addControlError(rSyntaxControl, result);
		else
		{
			Json::Reader parser;
			Json::Value jsonResult;
			parser.parse(fq(result), jsonResult);
			Json::Value options = jsonResult["options"];
			clearControlError(rSyntaxControl);
			clearFormErrors();
			if (_rSyntax->parseRSyntaxOptions(options))
			{
				blockValueChangeSignal(true);
				_analysis->clearOptions();
				bindTo(Json::nullValue);
				// Some controls generate extra controls (rowComponents): these extra controls must be first destroyed, because they may disturb the binding of other controls
				// For this, bind all controls to null and wait for the controls to be completely destroyed.
				QTimer::singleShot(0, [=](){
					bindTo(options);
					blockValueChangeSignal(false, false);
					_analysis->boundValueChangedHandler();
				});
			}
		}

		return;
	}

	JASPControl* item = getControl(controlName);
	if (!item)
	{
		QStringList composedName = controlName.split(".");
		if (composedName.length() == 3)
		{
			JASPControl* parentControl = getControl(composedName[0]);
			if (parentControl)
				item = dynamic_cast<JASPControl*>(parentControl->getChildControl(composedName[1], composedName[2]));
		}
	}

	if (item)
		item->rScriptDoneHandler(result);
	else
		Log::log() << "Unknown item " << controlName.toStdString() << std::endl;
}

void AnalysisForm::filterByNameDone(const QString & name, const QString & error)
{
	for(JASPControl * control : _controls)
		control->filterDoneHandler(name, error);
}

void AnalysisForm::addControl(JASPControl *control)
{
	const QString & name = control->name();

	if (_analysis && control->isBound())
	{
		connect(control, &JASPControl::requestColumnCreation, _analysis, &AnalysisBase::requestColumnCreationHandler);

		connect(control, &JASPControl::usedVariablesChanged, _analysis, &AnalysisBase::onUsedVariablesChanged);
	}

	if (control->controlType() == JASPControl::ControlType::Expander)
	{
		ExpanderButtonBase* expander = dynamic_cast<ExpanderButtonBase*>(control);
		_expanders.push_back(expander);
	}

	if (!name.isEmpty() && !control->nameIsOptionValue())
	{
		if (_controls.count(name) > 0)
		{
			control->addControlError(tr("2 controls have the same name: %1").arg(name));
			_controls[name]->addControlError(tr("2 controls have the same name: %1").arg(name));
		}
		else
			_controls[name] = control;
	}
	else if (name.isEmpty())
	{
		control->setUp();
		control->setInitialized();
	}
}

void AnalysisForm::addColumnControl(JASPControl* control, bool isComputed)
{
	if (isComputed)
	{
		connect(control, &JASPControl::requestComputedColumnCreation,		_analysis, &AnalysisBase::requestComputedColumnCreationHandler);
		connect(control, &JASPControl::requestComputedColumnDestruction,	_analysis, &AnalysisBase::requestComputedColumnDestructionHandler);
	}
	else
		connect(control, &JASPControl::requestColumnCreation, _analysis, &AnalysisBase::requestColumnCreationHandler);
}

void AnalysisForm::_setUpControls()
{
	_setUpModels();
	_setUp();
}

void AnalysisForm::_setUpModels()
{
	for (JASPControl* control : _controls.values())
	{
		JASPListControl*	listControl = qobject_cast<JASPListControl*>(control);
		if (listControl)	listControl->setUpModel();
	}
}

void AnalysisForm::sortControls(QList<JASPControl*>& controls)
{
	for (JASPControl* control : controls)
	{
		control->addExplicitDependency();
		std::vector<JASPControl*> depends(control->depends().begin(), control->depends().end());

		// By adding at the end of the vector new dependencies, this makes sure that these dependencies of these new dependencies are
		// added and so on recursively, so that the 'depends' set of each control gets all (direct or indirect) controls it depends on.
		// (that's why we cannot use a for-each loop here or an iterator, because it loops on a vector that is growing during the loop).
		// Afterwards, if a control depends (directly or indirectly) of another control, the number of elements in its 'depends' set is then
		// automatically strictly bigger than the 'depends' set of all controls it depends on.
		// We have then simply to use the size of their 'depends' set, to sort the controls.
		for (size_t index = 0; index < depends.size(); index++)
		{
			JASPControl					* depend		= depends[index];
			const std::set<JASPControl*>	& dependdepends = depend->depends();

			for (JASPControl* dependdepend : dependdepends)
				if (dependdepend == control)
					addFormError(tq("Circular dependency between control %1 and %2").arg(control->name()).arg(depend->name()));
				else if (control->addDependency(dependdepend))
					depends.push_back(dependdepend);
		}
	}

	std::sort(controls.begin(), controls.end(),
		[](JASPControl* a, JASPControl* b) {
			return a->depends().size() < b->depends().size();
		});
}

void AnalysisForm::setHasVolatileNotes(bool hasVolatileNotes)
{
	if (_hasVolatileNotes == hasVolatileNotes)
		return;

	_hasVolatileNotes = hasVolatileNotes;
	emit hasVolatileNotesChanged();
}


QString AnalysisForm::parseOptions(QString options)
{
	Json::Reader jsonReader;
	Json::Value	 jsonOptions;
	Json::Value jsonResult(Json::objectValue);

	
	jsonReader.parse(fq(options), jsonOptions, false);

	if (!_analysis)
		setAnalysis(new AnalysisBase(this)); // Create a dummy analyis object

	if (_rSyntax->parseRSyntaxOptions(jsonOptions))
	{
		bindTo(jsonOptions);
		jsonOptions = _analysis->boundValues();
	}

	jsonResult["options"] = jsonOptions;
	jsonResult["error"] = fq(getError());
	return tq(jsonResult.toStyledString());
}

void AnalysisForm::_setUp()
{
	QList<JASPControl*> controls = _controls.values();

	for (JASPControl* control : controls)
		control->setUp();

	sortControls(controls);

	for (JASPControl* control : controls)
	{
		_dependsOrderedCtrls.push_back(control);
		connect(control, &JASPControl::helpMDChanged, this, &AnalysisForm::helpMDChanged);
	}

	_rSyntax->setUp();

	emit helpMDChanged(); //Because we just got info on our lovely children in _orderedControls
}

void AnalysisForm::reset()
{
	_analysis->reloadForm();
}

void AnalysisForm::exportResults()
{
	_analysis->exportResults();
}

QString AnalysisForm::msgsListToString(const QStringList & list) const
{
	if(list.length() == 0)
		return "";

	if (list.size() == 1)
		return list[0];

	QString text;
	for (const QString & msg : list)
		if(msg.size())
			text.append("<li>").append(msg).append("</li>");

	return !text.size() ? "" : "<ul style=\"margins:0px\">" + text + "</ul>";
}

QString AnalysisForm::_getControlLabel(QString controlName)
{
	return _controls[controlName]->humanFriendlyLabel();
}

void AnalysisForm::_addLoadingError(QStringList wrongJson)
{
	if (wrongJson.size() > 0)
	{
		QString errorMsg;
		if (wrongJson.size() == 1)
		{
			errorMsg = tr("Component %1 was loaded with the wrong type of value and has been reset to its default value.").arg(_getControlLabel(wrongJson[0]));
			errorMsg += "<br>";
		}
		else if (wrongJson.size() < 4)
		{
			QString names = "<ul>";
			for(const QString & controlName : wrongJson)
				names += "<li>" + _getControlLabel(controlName) + "</li>";
			names += "</ul>";

			errorMsg = tr("These components were loaded with the wrong type of value and have been reset to their default values:%1").arg(names);
		}
		else
		{
			errorMsg = tr("Many components were loaded with the wrong type of value and have been reset to their default values.");
			errorMsg += "<br>";
		}

		errorMsg += tr("The file probably comes from an older version of JASP.");
		errorMsg += "<br>" + tr("That means that the results currently displayed do not correspond to the options selected.");
		errorMsg += "<br>" + tr("Refreshing the analysis may change the results.");
		addFormError(errorMsg);
	}
}

void AnalysisForm::bindTo(const Json::Value & defaultOptions)
{
	std::set<std::string> controlsJsonWrong;

	for (JASPControl* control : _dependsOrderedCtrls)
	{
		BoundControl* boundControl = control->boundControl();
		Json::Value optionValue = Json::nullValue;
		if (boundControl)
		{
			std::string name = control->name().toStdString();
			if (defaultOptions.isMember(name))
				optionValue = defaultOptions[name];

			if (optionValue != Json::nullValue && !boundControl->isJsonValid(optionValue))
			{
				optionValue = Json::nullValue;
				control->setHasWarning(true);
				controlsJsonWrong.insert(name);
			}
		}

		control->setInitialized(optionValue);
	}

	_addLoadingError(tql(controlsJsonWrong));

	//Ok we can only set the warnings on the components now, because otherwise _addLoadingError() will add a big fat red warning on top of the analysisform without reason...
	for (JASPControl* control : _dependsOrderedCtrls)
		control->addControlWarning(msgsListToString(tq(_analysis->upgradeMsgsForOption(fq(control->name())))));


	//Also check for a warning to show above the analysis:
	for(const QString & upgradeMsg : tq(_analysis->upgradeMsgsForOption("")))
		if(upgradeMsg != "")
			addFormWarning(upgradeMsg);
}

void AnalysisForm::addFormError(const QString & error)
{
	_formErrors.append(error);
	emit errorsChanged();
}

void AnalysisForm::addFormWarning(const QString & warning)
{
	_formWarnings.append(warning);
	emit warningsChanged();
}


//This should be moved to JASPControl maybe?
//Maybe even to full QML? Why don't we just use a loader...
void AnalysisForm::addControlError(JASPControl* control, QString message, bool temporary, bool warning, bool closeable)
{
	if (!control)
	{
		// Quite bad: write it at least to the log
		Log::log() << "Control error, but control not found: " << message << std::endl;
		return;
	}

	if (!message.isEmpty())
	{
		QQuickItem*	controlErrorMessageItem = nullptr;

		for (QQuickItem* item : _controlErrorMessageCache)
		{
			JASPControl* errorControl = item->property("control").value<JASPControl*>();
			if (errorControl == control || !errorControl)
			{
				controlErrorMessageItem = item;
				break;
			}
		}

		if (!controlErrorMessageItem)
		{
			// Cannot instantiate _controlErrorMessageComponent in the constructor (it crashes), and it might be too late in the formCompletedHandler since error can be generated earlier
			// So create it when it is needed for the first time.
			if (!_controlErrorMessageComponent)
				_controlErrorMessageComponent = new QQmlComponent(qmlEngine(this), "qrc:///components/JASP/Controls/ControlErrorMessage.qml");

			controlErrorMessageItem = qobject_cast<QQuickItem*>(_controlErrorMessageComponent->create(QQmlEngine::contextForObject(this)));
			if (!controlErrorMessageItem)
			{
				Log::log() << "Could not create Control Error Item!!" << std::endl;
				for (const QQmlError& error : _controlErrorMessageComponent->errors())
					Log::log() << "Error: " << error.description() << std::endl;
				return;
			}
			controlErrorMessageItem->setProperty("form", QVariant::fromValue(this));
			_controlErrorMessageCache.append(controlErrorMessageItem);
		}

		QQuickItem* container = this;
		if (control->parentListView())
		{
			container = control->parentListView()->property("listGridView").value<QQuickItem*>();
			if (!container)
				container = control->parentListView();
		}

		controlErrorMessageItem->setProperty("control", QVariant::fromValue(control));
		controlErrorMessageItem->setProperty("warning", warning);
		controlErrorMessageItem->setProperty("closeable", closeable);
		controlErrorMessageItem->setParentItem(container);
		QMetaObject::invokeMethod(controlErrorMessageItem, "showMessage", Qt::QueuedConnection, Q_ARG(QVariant, message), Q_ARG(QVariant, temporary));
	}

	if (warning)	control->setHasWarning(true);
	else			control->setHasError(true);
}

bool AnalysisForm::hasError()
{
	// _controls have only controls created when the form is created, not the ones created dynamically afterwards
	// So here we use a workaround: check whether one errorMessage item in _controlErrorMessageCache has a control (do not use visible since it becomes visible too late).
	// Controls handling inside a form must indeed be done in anther way!

	for (QQuickItem* item : _controlErrorMessageCache)
		if (item->property("control").value<JASPControl*>() != nullptr && !item->property("warning").toBool())
			return true;

	return false;
}

QString AnalysisForm::getError()
{
	QString message;

	for (QQuickItem* item : _controlErrorMessageCache)
		if (item->property("control").value<JASPControl*>() != nullptr)
			message += (message != "" ? ", " : "") + item->property("message").toString();

	return message;
}

void AnalysisForm::clearControlError(JASPControl* control)
{
	if (!control) return;

	for (QQuickItem * errorItem : _controlErrorMessageCache)
		if (control == errorItem->property("control").value<JASPControl*>())
			errorItem->setProperty("control", QVariant());

	control->setHasError(false);
	control->setHasWarning(false);
}

void AnalysisForm::clearFormErrors()
{
	_formErrors.clear();
	emit errorsChanged();

	for(auto & control : _controls)
	{
		control->setHasError(false);
		control->setHasWarning(false);
	}
}


void AnalysisForm::clearFormWarnings()
{
	_formWarnings.clear();
	emit warningsChanged();

	for(auto & control : _controls)
		control->setHasWarning(false);
}

void AnalysisForm::setAnalysis(AnalysisBase * analysis)
{
	if(_analysis == analysis) return;

	if(_analysis && analysis)
		throw std::runtime_error("An analysis of an analysisform was replaced by another analysis, this is decidedly NOT supported!");

	_analysis = analysis;

	Log::log() << "AnalysisForm " << this << " sets Analysis " << _analysis << " on itself" << std::endl;

	setAnalysisUp();
}

void AnalysisForm::boundValueChangedHandler(JASPControl *)
{
	if (_valueChangedSignalsBlocked == 0 && _analysis)
		_analysis->boundValueChangedHandler();
	else
		_valueChangedEmittedButBlocked = true;
}

void AnalysisForm::setTitle(QString title)
{
	if (_analysis)
		_analysis->setTitle(fq(title.simplified()));
}

void AnalysisForm::setOptionNameConversion(const QVariantList & conv)
{
	if (_rSyntax->setControlNameToRSyntaxMap(conv))
		emit optionNameConversionChanged();
}

void AnalysisForm::formCompletedHandler()
{
	Log::log() << "AnalysisForm::formCompletedHandler for " << this << " called." << std::endl;

	_formCompleted = true;
	setAnalysisUp();
}

void AnalysisForm::setAnalysisUp()
{
	if(!_formCompleted || !_analysis)
		return;

	Log::log() << "AnalysisForm::setAnalysisUp() for " << this << std::endl;

	blockValueChangeSignal(true);

	_setUpControls();

	Json::Value defaultOptions = _analysis->orgBoundValues();
	_analysis->clearOptions();
	bindTo(defaultOptions);

	blockValueChangeSignal(false, false);

	_initialized = true;

	// Don't bind boundValuesChanged before it is initialized: each setup of all controls will generate a boundValuesChanged
	connect(_analysis,					&AnalysisBase::boundValuesChanged,		this,			&AnalysisForm::setRSyntaxText,				Qt::QueuedConnection	);

	emit analysisChanged();
}

void AnalysisForm::knownIssuesUpdated()
{
	if(!_formCompleted || !_analysis)
		return;

	if(KnownIssues::issues()->hasIssues(_analysis->module(), _analysis->name()))
	{
		const std::vector<KnownIssues::issue> & issues = KnownIssues::issues()->getIssues(_analysis->module(), _analysis->name());

		for(const KnownIssues::issue & issue : issues)
		{
			for(const std::string & option : issue.options)
				if(_controls.count(tq(option)) > 0)
					_controls[tq(option)]->setHasWarning(true);

			_formWarnings.append(tq(issue.info));
		}

		emit warningsChanged();
	}
}

void AnalysisForm::setControlIsDependency(QString controlName, bool isDependency)
{
	if(_controls.count(controlName) > 0)
		_controls[controlName]->setProperty("isDependency", isDependency);
}

void AnalysisForm::setControlMustContain(QString controlName, QStringList containThis)
{
	if(_controls.count(controlName) > 0)
		_controls[controlName]->setProperty("dependencyMustContain", containThis);
}

void AnalysisForm::setMustBe(std::set<std::string> mustBe)
{
	if(mustBe == _mustBe)
		return;

	for(const std::string & mustveBeen : _mustBe)
		if(mustBe.count(mustveBeen) == 0)
			setControlIsDependency(mustveBeen, false);

	_mustBe = mustBe;

	for(const std::string & mustBecome : _mustBe)
		setControlIsDependency(mustBecome, true); //Its ok if it does it twice, others will only be notified on change
}

void AnalysisForm::setMustContain(std::map<std::string,std::set<std::string>> mustContain)
{
	if(mustContain == _mustContain)
		return;

	//For now ignore specific thing that must be contained
	for(const auto & nameContainsPair : _mustContain)
		if(mustContain.count(nameContainsPair.first) == 0)
			setControlMustContain(nameContainsPair.first, {});

	_mustContain = mustContain;

	for(const auto & nameContainsPair : _mustContain)
		setControlMustContain(nameContainsPair.first, nameContainsPair.second); //Its ok if it does it twice, others will only be notified on change

}

void AnalysisForm::setRunOnChange(bool change)
{
	if (change != _runOnChange)
	{
		_runOnChange = change;

		blockValueChangeSignal(change, false);

		emit runOnChangeChanged();
	}
}

void AnalysisForm::blockValueChangeSignal(bool block, bool notifyOnceUnblocked)
{
	if (block)
		_valueChangedSignalsBlocked++;
	else
	{
		_valueChangedSignalsBlocked--;

		if (_valueChangedSignalsBlocked < 0)
			_valueChangedSignalsBlocked = 0;

		if (_valueChangedSignalsBlocked == 0)
		{
			if(notifyOnceUnblocked && _analysis && _valueChangedEmittedButBlocked)
				_analysis->boundValueChangedHandler();

			_valueChangedEmittedButBlocked = false;

			if(_analysis && (notifyOnceUnblocked || _analysis->wasUpgraded())) //Maybe something was upgraded and we want to run the dropped rscripts (for instance for https://github.com/jasp-stats/INTERNAL-jasp/issues/1399)
			{
				while(_waitingRScripts.size() > 0)
				{
					const auto & front = _waitingRScripts.front();
					_analysis->sendRScript(std::get<0>(front), std::get<1>(front), std::get<2>(front));
					_waitingRScripts.pop();
				}

				for(const auto & filterName : _waitingFilters)
					_analysis->sendFilter(filterName);

				_waitingFilters.clear();
			}
			else //Otherwise just clean it up
			{
				_waitingRScripts = std::queue<std::tuple<QString, QString, bool>>();
				_waitingFilters.clear();
			}
		}
	}
}

QString AnalysisForm::rSyntaxText() const
{
	return _rSyntaxText;
}

bool AnalysisForm::needsRefresh() const
{
	return _analysis ? _analysis->needsRefresh() : false;
}

bool AnalysisForm::isFormulaName(const QString& name) const
{
	return _rSyntax->getFormula(name) != nullptr;
}

bool AnalysisForm::isColumnFreeOrMine(const QString &name) const
{
	return _analysis ? _analysis->isColumnFreeOrMine(name) : false;
}

QString AnalysisForm::generateRSyntax(bool useHtml) const
{
	return _rSyntax->generateSyntax(!useHtml && showAllROptions(), useHtml);
}

QVariantList AnalysisForm::optionNameConversion() const
{
	return _rSyntax->controlNameToRSyntaxMap();
}

std::vector<std::vector<string> > AnalysisForm::getValuesFromRSource(const QString &sourceID, const QStringList &searchPath)
{
	if (!_analysis) return {};

	const Json::Value& jsonSource = _analysis->getRSource(fq(sourceID));
	return  _getValuesFromJson(jsonSource, searchPath);
}

std::vector<std::vector<string> > AnalysisForm::_getValuesFromJson(const Json::Value& jsonValues, const QStringList& searchPath)
{
	auto getValueFromJson = [](const Json::Value& jsonValue) -> std::vector<std::string>
	{
		if (jsonValue.isString())			return {jsonValue.asString()};
		else if (jsonValue.isIntegral())	return {std::to_string(jsonValue.asInt())};
		else if (jsonValue.isNumeric())		return {std::to_string(jsonValue.asDouble())};
		else if (jsonValue.isArray())
		{
			std::vector<std::string> values;
			for (const Json::Value& oneValue: jsonValue)
			{
				if (oneValue.isString())	values.push_back(oneValue.asString());
				if (oneValue.isIntegral())	values.push_back(std::to_string(oneValue.asInt()));
				if (oneValue.isNumeric())	values.push_back(std::to_string(oneValue.asDouble()));
			}
			return values;
		}
		else return {};
	};

	std::vector<std::vector<string> > result;

	if (jsonValues.isNull())
		return result;

	if (!jsonValues.isArray() && !jsonValues.isObject())
		return {getValueFromJson(jsonValues)};

	QString path;
	QStringList nextPaths;
	if (searchPath.length() > 0)
	{
		path = searchPath[0];
		nextPaths = searchPath;
		nextPaths.removeAt(0);
	}

	if (jsonValues.isObject())
	{
		if (path.isEmpty())
		{
			std::vector<std::string> keys = jsonValues.getMemberNames();
			for (const std::string& key : keys)
				result.push_back({key});
		}
		else
		{
			std::string key = fq(path);
			if (jsonValues.isMember(key))
				result = _getValuesFromJson(jsonValues[key], nextPaths);
			else if (key == "values")
			{
				for (const Json::Value& jsonValue : jsonValues)
				{
					std::vector<std::vector<string> > values = _getValuesFromJson(jsonValue, nextPaths);
					if (values.size() > 0)
						result.push_back(values[0]);
				}
			}
			else
				Log::log() << "Key " << key << " not found in R source " << jsonValues.toStyledString() << std::endl;
		}
	}
	else // jsonValues is an array
	{
		bool pathIsIndex = false;
		uint index = path.isEmpty() ? 0 : path.toUInt(&pathIsIndex);

		if (pathIsIndex)
		{
			if (jsonValues.size() > index)
				result = _getValuesFromJson(jsonValues[index], nextPaths);
			else
				Log::log() << "Cannot retrieve values from R Source: index (" << index << ") bigger than size of the source (" << jsonValues.size() << ")" << std::endl;
		}
		else
		{
			for (const Json::Value& jsonValue : jsonValues)
			{
				if (path.isEmpty())
					result.push_back(getValueFromJson(jsonValue));
				else if (jsonValue.isObject())
				{
					std::string key = fq(path);
					if (jsonValue.isMember(key))
					{
						std::vector<std::vector<string> > values =_getValuesFromJson(jsonValue[key], nextPaths);
						if (values.size() > 0)
							result.push_back(values[0]);
					}
					else
						Log::log() << "Key " << key << " not found in R source " << jsonValue.toStyledString() << std::endl;
				}
				else
					Log::log() << "Caanot find path " << path << " in R source " << jsonValue.toStyledString() << std::endl;
			}
		}
	}

	return result;
}

void  AnalysisForm::setBoundValue(const string &name, const Json::Value &value, const Json::Value &meta, const QVector<JASPControl::ParentKey> &parentKeys)
{
	if (_analysis)
		_analysis->setBoundValue(name, value, meta, parentKeys);
}

std::set<string> AnalysisForm::usedVariables()
{
	std::set<string> result;

	for (JASPControl* control : _controls)
	{
		JASPListControl* listControl = qobject_cast<JASPListControl*>(control);
		if (listControl)
		{
			std::vector<std::string> usedVariables = listControl->usedVariables();
			result.insert(usedVariables.begin(), usedVariables.end());
		}
	}

	return result;
}

///Generates documentation based on the "info" entered on each component
QString AnalysisForm::helpMD() const
{
	if(!_analysis || !initialized()) return "";

	QStringList markdown =
	{
		"# ", title(), "\n",
		_info, "\n"
	};


	QList<JASPControl*> orderedControls = JASPControl::getChildJASPControls(this);
	orderedControls.removeIf([](JASPControl* c) { return c->helpMD().isEmpty(); });

	if (orderedControls.length() > 0 && orderedControls[0]->controlType() != JASPControl::ControlType::Expander)
		// If the first control is an ExpanderButton, then it adds already a line
		markdown << "\n---\n";

	for(JASPControl * control : orderedControls)
		markdown << control->helpMD() << "\n";

	markdown << metaHelpMD();

	if(!_infoBottom.isEmpty())
		markdown << "\n\n---\n" << _infoBottom  << "\n";

	QString md = markdown.join("");

	_analysis->preprocessMarkdownHelp(md);

	return md;
}

///Collects "info" from results and lists them underneath the output in the help-md window
QString AnalysisForm::metaHelpMD() const
{
	std::function<QString(const Json::Value & meta, int deep)> metaMDer = [&metaMDer](const Json::Value & meta, int deep)
	{
		QStringList markdown;

		for(const Json::Value & entry : meta)
		{
			std::string entryType	= entry.get("type", "").asString();
			//Sadly enough the following "meta-types" aren't defined properly anywhere, this would be good to do at some point. The types are: table, image, collection, and optionally: htmlNode, column, json
			QString friendlyObject	= entryType == "table"		? tr("Table")
									: entryType == "image"		? tr("Plot")
									: entryType == "collection"	? tr("Collection")
									: tr("Result"); //Anything else we just call "Result"

			if(entry.get("info", "") != "")
			{
				for(int i=0; i<deep; i++) markdown << "#";
				markdown << " " << friendlyObject;
				if(entry.get("title", "") != "")	markdown << tq(" - *" + entry["title"].asString() + "*:\n");
				else								markdown << "\n";
				markdown << tq(entry["info"].asString() + "\n");
			}

			if(entry.get("meta", Json::nullValue).isArray())
				markdown << "\n" << metaMDer(entry["meta"], deep + 1);
		}

		return markdown.join("");
	};

	QString meta = metaMDer(_analysis->resultsMeta(), 2).trimmed();
	return meta.isEmpty() ? "" : "---\n# " + tr("Output") + "\n\n" + meta;
}

void AnalysisForm::setShowRButton(bool showRButton)
{
	if (_showRButton == showRButton)
		return;

	_showRButton = showRButton;

	emit showRButtonChanged();
}

void AnalysisForm::setDeveloperMode(bool developerMode)
{
	if (_developerMode == developerMode)
		return;

	_developerMode = developerMode;

	emit developerModeChanged();
}

void AnalysisForm::setRSyntaxText()
{
	if (!initialized() || !PreferencesModelBase::preferences()->showRSyntax())
		return;

	QString text = generateRSyntax();

	if (text != _rSyntaxText)
	{
		_rSyntaxText = text;
		emit rSyntaxTextChanged();
	}
}

bool AnalysisForm::showAllROptions() const
{
	return PreferencesModelBase::preferences()->showAllROptions();
}

void AnalysisForm::setShowAllROptions(bool showAllROptions)
{
	PreferencesModelBase::preferences()->setShowAllROptions(showAllROptions);
}

void AnalysisForm::sendRSyntax(QString text)
{
	PreferencesModelBase::preferences()->setShowRSyntax(true);
	_analysis->sendRScript(text, rSyntaxControlName, false);
}

void AnalysisForm::toggleRSyntax()
{
	PreferencesModelBase* pref = PreferencesModelBase::preferences();
	pref->setShowRSyntax(!pref->showRSyntax());
}

void AnalysisForm::setActiveJASPControl(JASPControl* control, bool hasActiveFocus)
{
	bool emitSignal = false;
	if (hasActiveFocus)
	{
		 if (_activeJASPControl != control) emitSignal = true;
		 _activeJASPControl = control;
	}
	else if (control == _activeJASPControl)
	{
		 if (_activeJASPControl) emitSignal = true;
		 _activeJASPControl = nullptr;
	}

	if (emitSignal)
		emit activeJASPControlChanged();
}
