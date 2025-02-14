//
// Copyright (C) 2013-2021 University of Amsterdam
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

#include "formulasource.h"
#include "formulabase.h"
#include "analysisform.h"
#include "controls/jasplistcontrol.h"
#include "controls/sourceitem.h"
#include "models/listmodelassignedinterface.h"
#include "models/listmodelinteractionassigned.h"
#include "controls/rowcontrols.h"
#include "log.h"
#include "rsyntax.h"
#include "boundcontrols/boundcontrolterms.h"
#include "controls/componentslistbase.h"
#include "controls/variableslistbase.h"

FormulaSource::ExtraOption::ExtraOption(const QString& controlName, const QVariant &var)
{
	if (var.canConvert<QMap<QString, QVariant> >())
	{
		QMap<QString, QVariant> map = var.toMap();
		if (map.contains("optionName")) optionName = map["optionName"].toString();
		else							optionName = controlName;

		if (map.contains("useFormula")) useFormula = map["useFormula"].toBool();
	}
	else
		optionName = controlName;
}

FormulaSource::RandomEffects::RandomEffects(const QVariant &var)
{
	QMap<QString, QVariant> map;
	JASPControl* sourceControl = var.value<JASPControl*>();
	if (sourceControl)
		name = sourceControl->name();
	else if (var.canConvert<QString>())
		name = var.toString();
	else if (var.canConvert<QMap<QString, QVariant> >())
	{
		map = var.toMap();
		if (map.contains("id"))
		{
			sourceControl = map["id"].value<JASPControl*>();
			if (sourceControl) name = sourceControl->name();
		}

		if (map.contains("variablesSource"))	fixedEffectsSource = map["variablesSource"].toString();
		if (map.contains("variablesControl"))	variablesControl = map["variablesControl"].toString();
		if (map.contains("checkControl"))		checkControl = map["checkControl"].toString();
		if (map.contains("correlationControl"))	correlationControl = map["correlationControl"].toString();
		if (map.contains("variablesKey"))		variablesKey = map["variablesKey"].toString();
	}
}

FormulaSource::FormulaSource(FormulaBase* formula, const QVariant& var) : QObject(formula), _formula(formula)
{
	QMap<QString, QVariant> map;
	JASPControl* sourceControl = var.value<JASPControl*>();

	if (sourceControl)
		_sourceName = sourceControl->name();

	else if (var.canConvert<QString>())
		_sourceName = var.toString();
		
	else if (var.canConvert<QMap<QString, QVariant> >())
	{
		map = var.toMap();
		if (map.contains("id"))
		{
			sourceControl = map["id"].value<JASPControl*>();
			if (sourceControl) _sourceName = sourceControl->name();
		}
		else if (map.contains("randomEffects"))
		{
			_randomEffects = RandomEffects(map["randomEffects"]);
			_sourceName = _randomEffects.name;
		}
		else
			_sourceName = map["name"].toString();
	}

	if (_sourceName.isEmpty())
		_addError(tr("No name given in Formula description"));
	else
	{
		_model = _formula->getModel(_sourceName);
		if (!_model)
			_addError(tr("Cannot find item %1 given in Formula description").arg(_sourceName));
	}

	if (!_model) map.clear();

	if (map.contains("extraOptions"))
	{
		QList<QVariant> allExtraOptions = SourceItem::getListVariant(map["extraOptions"]);
		for (const QVariant& extraOptionVar : allExtraOptions)
		{
			QString controlName, optionName;
			if (extraOptionVar.canConvert<QString>())
				optionName = controlName = extraOptionVar.toString();
			else if (extraOptionVar.canConvert<QMap<QString, QVariant> >())
			{
				QMap<QString, QVariant> extraOptionMap = extraOptionVar.toMap();
				optionName = controlName = extraOptionMap["name"].toString();
				if (extraOptionMap.contains("controlName")) controlName = extraOptionMap["controlName"].toString();
			}
			if (!optionName.isEmpty())
				_extraOptions[controlName] = ExtraOption(optionName, extraOptionVar);
		}
	}

	if (!_randomEffects.isEmpty())
	{
		_randomEffects.fixedEffectsModel = _formula->form()->getModel(_randomEffects.fixedEffectsSource);
		if (!_randomEffects.fixedEffectsModel)
		{
			_randomEffects = RandomEffects();
			_addError(tr("No source found in Formula randomEffects"));
		}
		_randomEffects.componentsList = qobject_cast<ComponentsListBase*>(_model->listView());
		if (!_randomEffects.componentsList)
		{
			_addError(tr("randomEffects in Formula can be used only with a ComponentsList item"));
			_randomEffects = RandomEffects();
		}
	}
}

QVector<FormulaSource*> FormulaSource::makeFormulaSources(FormulaBase* formula, const QVariant& var)
{
	QVector<FormulaSource*> result;
	QList<QVariant> allFormulaSources = SourceItem::getListVariant(var);

	for (const QVariant& formulaSourceVar : allFormulaSources)
		if (!formulaSourceVar.isNull())
		{
			FormulaSource* formulaSource = new FormulaSource(formula, formulaSourceVar);
			// formulaSource with randomEffetcs is set last in the list, in order to be treated after the fixed effects
			if (formulaSource->hasRandomEffects())	result.push_back(formulaSource);
			else									result.push_front(formulaSource);
		}

	return result;
}

QStringList FormulaSource::modelSources() const
{
	QStringList result;
	if (_model == nullptr)	return result;

	auto findSources = [] (ListModel* model, QStringList &sources)
	{
		sources.append(model->name());
		const QVector<SourceItem*>& sourceItems = model->listView()->sourceItems();
		for (SourceItem* sourceItem : sourceItems)
		{
			ListModel* listModel = sourceItem->sourceListModel();
			if (listModel) sources.append(listModel->name());
		}
	};

	auto findSourcesWithAvailable = [=] (ListModel* model, QStringList &sources)
	{
		findSources(model, sources);
		ListModelAssignedInterface* assignedModel = qobject_cast<ListModelAssignedInterface*>(model);
		if (assignedModel)
		{
			ListModelAvailableInterface* availableModel = assignedModel->availableModel();
			if (availableModel) findSources(availableModel, sources);
		}

	};

	findSourcesWithAvailable(_model, result);

	if (_randomEffects.fixedEffectsModel)
		findSourcesWithAvailable(_randomEffects.fixedEffectsModel, result);

	return result;
}

QString FormulaSource::toString() const
{
	if (!_model)										return tr("No source found in Formula");

	const Terms& terms = _model->terms();
	Json::Value changedTypes = _model->getVariableTypes(true);

	if (hasRandomEffects())								return _generateRandomEffectsTerms(terms);
	if (_model->listView()->containsInteractions())		return generateInteractionTerms(terms, changedTypes);
	else												return _generateSimpleTerms(terms, changedTypes);
}


QString FormulaSource::_generateRandomEffectsTerms(const Terms& terms) const
{
	QString result;

	QMap<QString, RowControls*> allRowControls = _model->getAllRowControls();
	for (const Term& term : terms)
	{
		QString key = term.asQString();
		if (!allRowControls.contains(key))
		{
			Log::log() << "Cannot find key " << key << " in rowControls for RandomEffetcs Source" << std::endl;
			continue;
		}
		RowControls* rowControls = allRowControls[key];
		JASPControl* correlationControl = rowControls && !_randomEffects.correlationControl.isEmpty() ? rowControls->getJASPControl(_randomEffects.correlationControl) : nullptr;
		JASPListControl* variablesListControl = qobject_cast<JASPListControl*>(rowControls && !_randomEffects.variablesControl.isEmpty() ? rowControls->getJASPControl(_randomEffects.variablesControl) : nullptr);

		if (!variablesListControl)
		{
			Log::log() << "Cannot find component list " << _randomEffects.variablesControl << " in RandomEffetcs terms!" << std::endl;
			continue;
		}
		ListModel* componentListModel = variablesListControl->model();
		bool hasCorrelation = correlationControl ? correlationControl->property("checked").toBool() : false;

		Terms filteredTerms = _randomEffects.checkControl.isEmpty() ? componentListModel->terms() : SourceItem::filterTermsWithCondition(componentListModel, componentListModel->terms(), _randomEffects.checkControl);
		bool hasIntercept = _checkIntercept(filteredTerms);

		if (filteredTerms.size() > 0 || hasIntercept)
		{
			if (!result.isEmpty()) result += " + " ;

			result += "(";
			result += (hasIntercept ? "1" : "0");

			if (filteredTerms.size() > 0)
				result += " + " + generateInteractionTerms(filteredTerms, componentListModel->getVariableTypes(filteredTerms, true));

			result += (hasCorrelation ? " | " : " || ");
			result += key;
			result += ")";
		}
	}

	return result;
}

QString FormulaSource::generateInteractionTerms(const Terms& terms, const Json::Value& changedTypes)
{
	// If the terms has interactions, try to use the '*' symbol when all combinations of the subterms are also present in the terms.
	QString result;
	bool first = true;
	std::vector<Term> sortedTerms = terms.terms();
	QMap<Term, Json::Value> termsToAdd;
	std::sort(sortedTerms.begin(), sortedTerms.end(), [](const Term& a, const Term& b){ return a.components().length() < b.components().length(); });
	std::vector<Term> orgTerms = sortedTerms;

	// First try to find out whether if several terms can be reduced in a crass combination term: "a * b" means "a + b + a:b"
	while (!sortedTerms.empty())
	{
		const Term& term = sortedTerms.at(sortedTerms.size() - 1);
		int orgInd = terms.indexOf(term);
		const Json::Value& changedType = changedTypes.size() > orgInd ? changedTypes[orgInd] : Json::nullValue;
		sortedTerms.pop_back();
		if (term.components().size() == 1)
			termsToAdd[term] = changedType;
		else
		{
			bool allComponentsAreAlsoInTerms = true;
			Terms allCrossedTerms = Terms(term.components()).crossCombinations();
			allCrossedTerms.remove(term);
			for (const Term& oneTerm : allCrossedTerms)
			{
				if (std::find(orgTerms.begin(), orgTerms.end(), oneTerm) == orgTerms.end())
				{
					allComponentsAreAlsoInTerms = false;
					break;
				}
			}

			if (allComponentsAreAlsoInTerms)
			{
				// Remove the components and their combinations in terms so that they don't appear in the formula
				for (const Term& oneTerm : allCrossedTerms)
				{
					auto found = std::find(sortedTerms.begin(), sortedTerms.end(), oneTerm);
					if (found != sortedTerms.end()) sortedTerms.erase(found);
				}

				if (!first)	result += " + ";
				result += FormulaParser::transformToFormulaTerm(term, changedType, FormulaParser::allInterationsSeparator);
				first = false;
			}
			else
				termsToAdd[term] = changedType;
		}
	}

	// Use the original order of the terms to add them in the formula
	for (const Term term : terms)
	{
		if (termsToAdd.contains(term))
		{
			if (!first)	result += " + ";
			result += FormulaParser::transformToFormulaTerm(term, termsToAdd[term], FormulaParser::interactionSeparator);
			first = false;
		}
	}

	return result;
}


QString FormulaSource::_generateSimpleTerms(const Terms &terms, const Json::Value& types) const
{
	QString result;
	int i = 0;

	for (const Term& term : terms)
	{
		Json::Value changedType = types.size() > i ? types[i] : Json::nullValue;
		if (i > 0) result += " + ";
		result += FormulaParser::transformToFormulaTerm(term, changedType, FormulaParser::interactionSeparator);
		i++;
	}

	return result;
}

std::pair<Terms, Json::Value> FormulaSource::_onlyTrueTerms(const QString& controlName) const
{
	Terms onlyTrueTerms;
	Json::Value onlyTrueTypes(Json::arrayValue);

	if (!_model) return std::make_pair(onlyTrueTerms, onlyTrueTypes);

	Terms terms = _model->terms();
	Json::Value changedTypes = _model->getVariableTypes(true);

	int i = 0;
	for (const Term& term : terms)
	{
		const Json::Value& changedType = changedTypes.size() > i ? changedTypes[i] : Json::nullValue;
		JASPControl* control = _model->getRowControl(term.asQString(), controlName);
		BoundControl* boundControl = control ? control->boundControl() : nullptr;
		if (boundControl && boundControl->boundValue().asBool())	
		{
			onlyTrueTerms.add(term);
			onlyTrueTypes.append(changedType);
		}
		i++;
	}

	return std::make_pair(onlyTrueTerms, onlyTrueTypes);
}

ListModel::RowControlsValues  FormulaSource::_getTermsFromExtraOptions(const Json::Value& options) const
{
	QMap<QString, QMap<QString, Json::Value> > extraTermsMap;
	for (const QString& extraControlName : _extraOptions.keys())
	{
		const Json::Value& extraOptionJson = options[fq(_extraOptions[extraControlName].optionName)];
		if (extraOptionJson.isObject())
		{
			FormulaParser::ParsedTerms parsedTerms;
			QString error;

			if (FormulaParser::parse(extraOptionJson["rhs"], false, parsedTerms, error))
			{
				for (const Term& parsedTerm : parsedTerms.fixedTerms)
					extraTermsMap[parsedTerm.asQString()][extraControlName] = true;
			}
		}
		else if (extraOptionJson.isString())
		{
				extraTermsMap[tq(extraOptionJson.asString())][extraControlName] = true;
		}
		else if (extraOptionJson.isArray())
		{
			for (const Json::Value& jsonTerm : extraOptionJson)
			{
				if (jsonTerm.isString()) extraTermsMap[tq(jsonTerm.asString())][extraControlName] = true;
				else if (jsonTerm.isArray())
				{
					QStringList components;
					for (const Json::Value& jsonComponent : jsonTerm)
						if (jsonComponent.isString())
							components.append(tq(jsonComponent.asString()));
					extraTermsMap[Term(components).asQString()][extraControlName] = true;
				}
			}
		}
		else
			Log::log() << "Impossible Json type in _addTermsToOptions: " << extraOptionJson.toStyledString() << std::endl;
	}

	return extraTermsMap;

}

const QString FormulaSource::interceptTerm = "Intercept";

bool FormulaSource::_checkIntercept(Terms &terms) const
{
	bool hasIntercept = false;
	for (const Term& term : terms)
	{
		if (term.asQString() == interceptTerm)
		{
			hasIntercept = true;
			terms.remove(term);
			break;
		}
	}

	return hasIntercept;
}

void FormulaSource::_addTermsToOptions(ListModelAssignedInterface *model, Json::Value &options, const Terms &terms) const
{
	BoundControlTerms* boundControl = dynamic_cast<BoundControlTerms*>(model->listView()->boundControl());
	if (!boundControl)
	{
		Log::log() << "Add terms to options with a view that has not a BoundControlBase!!" << std::endl;
		return;
	}

	std::string optionName = fq(model->name());
	ListModel::RowControlsValues extraTermsMap;

	if (model == _model && _extraOptions.size() > 0) extraTermsMap = _getTermsFromExtraOptions(options);

	options[optionName] = boundControl->addTermsToOption(options[optionName], terms, extraTermsMap);
}

bool FormulaSource::_areTermsInOptions(ListModelAssignedInterface *model, const Json::Value &options, Terms &terms) const
{
	BoundControlTerms* boundControl = dynamic_cast<BoundControlTerms*>(model->listView()->boundControl());
	std::string optionName = fq(model->name());

	return boundControl ? boundControl->areTermsInOption(options[optionName], terms) : false;
}

FormulaParser::ParsedTerms FormulaSource::fillOptionsWithParsedTerms(const FormulaParser::ParsedTerms& parsedTerms, Json::Value& options) const
{
	if (_randomEffects.isEmpty())	return _fillOptionsWithFixedTerms(_model, parsedTerms, options);
	else							return _fillOptionsWithRandomTerms(parsedTerms, options);
}

FormulaParser::ParsedTerms FormulaSource::_fillOptionsWithFixedTerms(ListModel* model, const FormulaParser::ParsedTerms& parsedTerms, Json::Value& options, QMap<QString, Terms>* termsMap) const
{
	FormulaParser::ParsedTerms			remainingParsedTerms;
	QList<ListModelAssignedInterface*>	unspecifiedSourceModels,
										specifiedSourceModels;
	QStringList							specifiedByUserSources = _formula->sourcesThatMustBeSpecified();

	if (_sourceName.isEmpty())	return parsedTerms;

	ListModelAssignedInterface* assignedModel = qobject_cast<ListModelAssignedInterface*>(model);

	if (!assignedModel)
	{
		Log::log() << "Not an assigned model in fillOptionsWithParsedTerms..." << std::endl;
		return parsedTerms;
	}

	options[fq(assignedModel->name())] = Json::Value::null;
	bool isInteractionModel = qobject_cast<ListModelInteractionAssigned*>(assignedModel) != nullptr;
	const QVector<SourceItem*>& sourceItems = model->listView()->sourceItems();
	for (SourceItem* sourceItem : sourceItems)
	{
		ListModelAssignedInterface* assignedSourceModel = qobject_cast<ListModelAssignedInterface*>(sourceItem->sourceListModel());
		if (assignedSourceModel)
		{
			if (specifiedByUserSources.contains(assignedSourceModel->name()))	specifiedSourceModels.push_back(assignedSourceModel);
			else																unspecifiedSourceModels.push_back(assignedSourceModel);
		}
	}

	ListModelAvailableInterface* availableModel = assignedModel->availableModel();
	if (availableModel)
	{
		const QVector<SourceItem*>& sourceItems = availableModel->listView()->sourceItems();
		for (SourceItem* sourceItem : sourceItems)
		{
			ListModelAssignedInterface* assignedSourceModel = qobject_cast<ListModelAssignedInterface*>(sourceItem->sourceListModel());
			if (assignedSourceModel)
			{
				if (specifiedByUserSources.contains(assignedSourceModel->name()))	specifiedSourceModels.push_back(assignedSourceModel);
				else																unspecifiedSourceModels.push_back(assignedSourceModel);
			}
		}
	}

	remainingParsedTerms.intercept = parsedTerms.intercept;
	remainingParsedTerms.randomTerms = parsedTerms.randomTerms;
	for (const Term& parsedTerm : parsedTerms.fixedTerms)
	{
		if (parsedTerms.randomTerms.contains(parsedTerm.asQString()))
		{
			remainingParsedTerms.fixedTerms.add(parsedTerm);
			continue;
		}
		bool found = false;
		Terms terms, termsToSearch;
		terms.add(parsedTerm);
		if (isInteractionModel)
		{
			int i = 0;
			for (const QString component : parsedTerm.components())
			{
				columnType type = parsedTerm.types().size() > i ? parsedTerm.types()[i] : columnType::unknown;
				termsToSearch.add(Term(component, type));
				i++;
			}
		}
		else
			 termsToSearch = terms;

		if (termsToSearch.size() == 0) continue;

		// Check first whether the terms come from a user specified source first.
		// In this case, as the model must be explicitly specified by the user, the model should not be changed.
		for (ListModelAssignedInterface* model : specifiedSourceModels)
		{
			found = _areTermsInOptions(model, options, termsToSearch);
			if (found) break;
		}

		if (!found)
		{
			// If the terms does not come from the user specified sources, check if they are in a unspecified source model
			// In this case the terms have to be added in this source.
			for (ListModelAssignedInterface* sourceModel : unspecifiedSourceModels)
			{
				if (sourceModel->availableModel() != nullptr)
				{
					found = true;
					const Terms& availableTerms = sourceModel->availableModel()->allTerms();
					for (const Term& term : termsToSearch)
						if (!availableTerms.contains(term))
							found = false;

					if (found)
					{
						terms = sourceModel->checkTermsTypes(terms);
						termsToSearch = sourceModel->checkTermsTypes(termsToSearch);
						_addTermsToOptions(sourceModel, options, termsToSearch);
						break;
					}
				}
			}

		}

		if (!found && availableModel)
		{
			// If the terms do not come from the source models, it may then come from the available list.
			found = true;
			const Terms& availableTerms = availableModel->allTerms();
			for (const Term& term : termsToSearch)
				if (!availableTerms.contains(term))
					found = false;
		}

		if (found)
		{
			_addTermsToOptions(assignedModel, options, terms);
			if (termsMap)
			{
				Terms currentTerms = (*termsMap)[assignedModel->name()];
				currentTerms.add(terms);
				(*termsMap)[assignedModel->name()] = currentTerms;
			}
		}
		else
			remainingParsedTerms.fixedTerms.add(parsedTerm);
	}

	return remainingParsedTerms;
}

FormulaParser::ParsedTerms FormulaSource::_fillOptionsWithRandomTerms(const FormulaParser::ParsedTerms& parsedTerms, Json::Value& options)	const
{
	FormulaParser::ParsedTerms	remainingParsedTerms;

	VariablesListBase* fixedEffectsControl = qobject_cast<VariablesListBase*>(_randomEffects.fixedEffectsModel->listView());
	fixedEffectsControl->bindTo(options[fq(_randomEffects.fixedEffectsSource)]);
	Terms	fixedEffectTerms = fixedEffectsControl->model()->terms();

	QList<ListModelAssignedInterface*>	sourceModels;

	const QVector<SourceItem*>& sourceItems = _model->listView()->sourceItems();
	for (SourceItem* sourceItem : sourceItems)
	{
		ListModelAssignedInterface* assignedSourceModel = qobject_cast<ListModelAssignedInterface*>(sourceItem->sourceListModel());
		if (assignedSourceModel)
			sourceModels.push_back(assignedSourceModel);
	}

	std::map<ListModelAssignedInterface*, Terms> sourceMainTermsMap;
	ListModel::RowControlsValues randomTermsMap;
	Terms mainTerms;

	for (auto i = parsedTerms.randomTerms.begin(); i != parsedTerms.randomTerms.end(); ++i)
	{
		Term mainTerm = Term(i.key(), columnType::nominal);
		mainTerms.add(mainTerm);
		const FormulaParser::RandomTerm& randomTerm = i.value();

		for (ListModelAssignedInterface* model : sourceModels)
		{
			if (model->availableModel() != nullptr)
			{
				const Terms& availableTerms = model->availableModel()->allTerms();
				if (availableTerms.contains(mainTerm))
				{
					Terms sourceMainTerms = sourceMainTermsMap[model];
					sourceMainTerms.add(mainTerm);
					sourceMainTermsMap[model] = sourceMainTerms;
					break;
				}
			}
		}

		QMap<QString, Json::Value> componentValues;
		Terms variables;
		variables.add(interceptTerm);

		ListModel::RowControlsValues controlValues;
		QMap<QString, Json::Value> interceptCheck;
		interceptCheck[_randomEffects.checkControl] =  randomTerm.intercept;
		controlValues[interceptTerm] = interceptCheck;

		for (const Term& fixedTerm : fixedEffectTerms)
		{
			variables.add(fixedTerm);
			QMap<QString, Json::Value> checkValue;
			checkValue[_randomEffects.checkControl] = randomTerm.terms.contains(fixedTerm);
			controlValues[fixedTerm.asQString()] = checkValue;
		}
		componentValues[_randomEffects.correlationControl] = randomTerm.correlated;
		componentValues[_randomEffects.variablesControl] = BoundControlTerms::makeOption(variables, controlValues, fq(_randomEffects.variablesKey), true, true, false);
		randomTermsMap[mainTerm.asQString()] = componentValues;
	}

	for (auto sourceMainTermsIt : sourceMainTermsMap)
		_addTermsToOptions(sourceMainTermsIt.first, options, sourceMainTermsIt.second);

	options[fq(_sourceName)] = _randomEffects.componentsList->getJsonFromComponentValues(mainTerms, randomTermsMap);

	//Log::log() << "TATA: options " << options.toStyledString() << std::endl;

	return remainingParsedTerms;
}

void FormulaSource::_addError(const QString &error) const
{
	_formula->rSyntax()->addError(error);
}

// If a model is used in a formula, its row controls must be set as extra options
QMap<QString, QString> FormulaSource::additionalOptionStrings() const
{
	QMap<QString, QString> result;
	if (!_model || hasRandomEffects()) return result;

	const QList<RowControls*>& rowControls = _model->getAllRowControls().values();
	if (rowControls.size() == 0) return result;

	// Loop on all row controls. To do this, take the controls of the first row.
	const QMap<QString, JASPControl*>& firstRowControls = rowControls[0]->getJASPControlsMap();
	for (const QString& controlName : firstRowControls.keys())
	{
		BoundControl* boundControl = firstRowControls[controlName]->boundControl();
		if (!boundControl) continue; // Only boundControl can get extra options

		const ExtraOption& extraOption = _extraOptions.contains(controlName) ? _extraOptions[controlName] : ExtraOption(controlName);
		bool valueIsBool = boundControl->boundValue().type() == Json::booleanValue;
		std::pair<Terms, Json::Value> termsWithChangedTypes = valueIsBool ? _onlyTrueTerms(controlName) : std::make_pair(_model->terms(), _model->getVariableTypes(true));
		if (termsWithChangedTypes.first.size() == 0) continue;

		QString optionValue;
		const Terms& terms = termsWithChangedTypes.first;
		const Json::Value& changedTypes = termsWithChangedTypes.second;

		if (valueIsBool && extraOption.useFormula)
			optionValue = "~ " + generateInteractionTerms(terms, changedTypes);
		else
		{
			optionValue = "list(";
			int i = 0;
			for (const Term& term : terms)
			{
				const Json::Value& changedType = changedTypes.size() > i ? changedTypes[i] : Json::nullValue;
				if (i > 0) optionValue += ", ";

				if (valueIsBool)	optionValue += FormulaParser::transformToFormulaTerm(term, changedType, FormulaParser::allInterationsSeparator, true);
				else				optionValue += FormulaParser::transformToFormulaTerm(term, changedType, FormulaParser::allInterationsSeparator, true) + " = " + RSyntax::transformJsonToR(boundControl->boundValue());
				i++;
			}
			optionValue += ")";
		}

		result[extraOption.optionName] = optionValue;
	}

	return result;
}

bool FormulaSource::isEmpty() const
{
	return _model ? _model->terms().size() == 0 : true;
}

QStringList FormulaSource::extraOptions(bool useOptionName, bool onlyFormula) const
{
	QStringList result;

	for (const QString& controlName : _extraOptions.keys())
	{
		const ExtraOption& extraOption = _extraOptions[controlName];

		if (onlyFormula && !extraOption.useFormula) continue;
		if (useOptionName)	result.push_back(extraOption.optionName);
		else				result.push_back(controlName);
	}

	return result;
}

std::string FormulaSource::_controlToOptionName(const QString &name) const
{
	return fq(_formula->rSyntax()->getRSyntaxFromControlName(name));
}
