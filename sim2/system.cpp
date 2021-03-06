#include <algorithm>
#include "system.h"
#include "tools.h"
#include <iostream>
#include <assert.h>

Job::Job() 
{
	m_difficulty = 1.0f;

	m_bestCorrectness = 0;
	m_assumedCorrectness = 0;
}

float Job::getCorrectness() const
{
	return m_assumedCorrectness + m_bestCorrectness;
}

void Job::nodeStarted( AssumedResult* res )
{
	m_assumedCorrectness += res->correctness;
	m_assumedResults[res->node] = res;
}

Result* Job::workDone( AssumedResult* res, int hash )
{
	m_assumedResults.erase(res->node);
	m_assumedCorrectness -= res->correctness;
	assert(m_assumedCorrectness >= -0.01f);
	if(m_bestCorrectness >= 1.0f) {
		//This job was already done, this is late send (?)
		return NULL;
	}

	Result* result = new Result();
	result->hash = hash;
	result->node = res->node;
	result->job = this;
	result->correctness = res->correctness;

	m_results.insert(std::pair<int, Result*>(hash, result));
	auto it = m_correctnessPerHash.find(hash);
	float resultCor = result->correctness;
	if(it != m_correctnessPerHash.end()) {
		resultCor += it->second;
	}

	m_correctnessPerHash[hash] = resultCor;

	if(resultCor > m_bestCorrectness) {
		m_bestCorrectness = resultCor;
	}

	//Job is now done, hand out trust
	if(m_bestCorrectness >= 1.0f) {
		//This job is done now, add trust to all participants
		for(auto it = m_results.begin(); it != m_results.end(); ++it) {
			auto res = it->second;
			auto node = res->node;

			if(res->hash == hash) {
				node->m_trust += m_bestCorrectness - res->correctness;
			}
		}

		if(hash != 0) {
			std::cout << "Incorrect result got accepted." << std::endl;
			exit(1);
		}
	}

	return result;
}

Node::Node() 
{
	m_trust = 0;
	m_performance = 0.0f;

	m_nextActionTime = 0;
	m_lastActionTime = 0;
	m_currentWork = NULL;

	m_falseRatio = 0.0f;
}

bool Node::hasSubmitted( Job* job )
{
	return m_resultsJob.find(job) != m_resultsJob.end();
}

void Node::startJob( Job* job, float corr, uint64_t currentTick )
{
	AssumedResult* assumed = new AssumedResult();
	assumed->job = job;
	assumed->node = this;
	assumed->correctness = corr;

	job->nodeStarted(assumed);

	m_currentWork = assumed;

	m_nextActionTime = currentTick + 1 + (uint64_t)(100.f * job->m_difficulty * (1.0f - m_performance));
}

void Node::endJob()
{
	int hash = 0;

	if(m_falseRatio > 0.0f) {
		if(randf() < m_falseRatio) {
			hash = rand();
		}
	}

	auto res = m_currentWork->job->workDone(m_currentWork, hash);
	if(res) {
		m_results.push_back(res);
		m_resultsJob.insert(res->job);
	}
	m_currentWork = NULL;
}

float round2(float f,float pres)
{
	return (float) (floor(f*(1.0f/pres) + 0.5)/(1.0f/pres));
}

bool JobCompare::operator()( const Job* a, const Job* b ) const
{
	if(a == b) {
		return false;
	}

	if(a->m_active == b->m_active) {
		float ac = a->getCorrectness();
		float bc = b->getCorrectness();
		if(ac == bc) {
			return a < b;
		}

		return ac > bc;
	} else {
		return a->m_active;
	}
}

bool NodeCompare::operator()( const Node* a, const Node* b ) const
{
	if(a == b) {
		return false;
	}

	if(a->m_nextActionTime == b->m_nextActionTime) {
		return a < b; //compare ptrs
	}

	return a->m_nextActionTime < b->m_nextActionTime;
}

Project::Project()
{
	m_bestTrust = 0.0f;

	quiet = false;
	nograph = false;
}

void Project::addNode( Node* node )
{
	m_nodes.insert(node);
	updateTrust(node);

}

Job* Project::findJobForNode( Node* node )
{
	Job search;
	search.m_assumedCorrectness = clamp(getRand(1.0f, 1.3f) - getTrust(node), 0.0f, 1.0f);
	search.m_active = true;

	auto it = std::lower_bound(m_jobs.begin(), m_jobs.end(), &search, JobCompare());
	for(; it != m_jobs.end(); ++it) {
		auto obj = *it;
		if(!obj->m_active) {
			break;
		}

		if(obj->getCorrectness() >= 1.0f) {
			continue;
		}

		if(node->hasSubmitted(obj)) {
			continue;
		}

		m_jobs.erase(it);
		return obj;
	}

	search.m_assumedCorrectness = 1.0f;

	it = std::lower_bound(m_jobs.begin(), m_jobs.end(), &search, JobCompare());
	for(; it != m_jobs.end(); ++it) {
		auto obj = *it;
		if(!obj->m_active) {
			break;
		}

		if(obj->getCorrectness() >= 1.0f) {
			continue;
		}

		if(node->hasSubmitted(obj)) {
			continue;
		}

		m_jobs.erase(it);
		return obj;
	}

	return NULL;
}

float Project::getTrust( Node* node )
{
	float constTrust = 0.1f;

	if(m_bestTrust == 0.0f) {
		return constTrust;
	}

	assert(node->m_trust <= m_bestTrust);
	return clamp(constTrust + node->m_trust / m_bestTrust, 0.0f, 1.0f);
}

void Project::updateTrust( Node* node )
{
	if(node->m_trust > m_bestTrust) {
		m_bestTrust = node->m_trust;
	}
}

#include "../gnuplot-iostream/gnuplot-iostream.h"

void Project::activateJob()
{
	auto it = m_jobs.rbegin();
	auto obj = *it;

	if(obj->m_active) {
		return;
	}

	m_jobs.erase(--(it.base()));
	obj->m_active = true;
	m_jobs.insert(obj);
}

void Project::simulate()
{
	Gnuplot gp;

	typedef std::vector<std::pair<uint64_t, float> > plot_t;

	plot_t avg_confs;

	struct plots_t {
		int id;

		plot_t trust;
		plot_t trust_abs;
		plot_t jobs;
	};

	std::unordered_map<Node*, plots_t> plots;

	uint64_t currentTick = 0;
	int resultsSent = 0;
	int jobsDone = 0;

	auto node_it = m_nodes.begin();
	std::advance(node_it, 5);
	
	//(*node_it)->m_falseRatio = 0.3f;

	auto nodee = (*node_it);
	m_nodes.erase(node_it++);
	//nodee->m_lastActionTime = 250;
	plots[nodee] = plots_t();
	plots[nodee].id = 1;
	m_nodes.insert(nodee);

	for(int i = 1; i < 3; i++) {
		plots[*node_it] = plots_t();
		plots[*node_it].id = i+1;

		node_it++;
	}

	int noWorkFor = 0;

	std::set<Node*> nodesToReinsert;
	for(;; currentTick++) {
		bool foundWork = false;
		for(auto it = m_nodes.begin(); it != m_nodes.end(); ) {
			auto node = *it;
			if(node->m_nextActionTime > currentTick) {
				break;
			}

			if(node->m_lastActionTime != 0 && node->m_lastActionTime < currentTick && !node->m_currentWork) {
				continue;
			}

			m_nodes.erase(it++);

			auto node_plot_it = plots.find(node);
			plots_t* node_plot = (node_plot_it != plots.end() ? &node_plot_it->second : NULL);

			if(node->m_currentWork) {
				Job* currentJob = node->m_currentWork->job;
				auto job_it = m_jobs.find(currentJob);
				m_jobs.erase(job_it);
				
				bool done = currentJob->m_bestCorrectness >= 1.0f;

				node->endJob();

				m_jobs.insert(currentJob);

				resultsSent++;
				if(!done && currentJob->m_bestCorrectness >= 1.0f) {
					activateJob();
					jobsDone++;
				}

				for(auto node_it = currentJob->m_results.begin(); node_it != currentJob->m_results.end(); ++node_it) {
					updateTrust(node_it->second->node);
				}

				if(node->m_lastActionTime != 0 && node->m_lastActionTime < currentTick) {
					continue;
				}
			}

			auto job = findJobForNode(node);
			if(job) {
				if(node_plot) {
					node_plot->jobs.push_back(std::pair<uint64_t, float>(currentTick, getTrust(node)));
				}

				float corr = std::max<float>(0.0f, getTrust(node) + getRand(-0.1f, 0.1f));
				corr = std::min<float>(corr, 0.99f);

				node->startJob(job, corr, currentTick);
				m_jobs.insert(job);

				foundWork = true;
			} else {
				//no job - delay a tick
				node->m_nextActionTime = currentTick;
			}
			
			nodesToReinsert.insert(node);
		}

		if(!foundWork) {
			noWorkFor++;
			if(noWorkFor > 1000) {
				std::cout << "No jobs assigned for 1000 ticks, bailing out." << std::endl;
				return;
			}
		}

		for(auto plotit = plots.begin(); plotit != plots.end(); ++plotit) {
			plotit->second.trust.push_back(std::pair<uint64_t, float>(currentTick, getTrust(plotit->first)));
			plotit->second.trust_abs.push_back(std::pair<uint64_t, float>(currentTick, plotit->first->m_trust));
		}


		if(!nodesToReinsert.empty()) {
			for(auto it = nodesToReinsert.begin(); it != nodesToReinsert.end(); ++it) {
				addNode(*it);
			}

			nodesToReinsert.clear();

			if(!quiet) {
				std::cout << "tick: " << currentTick << " jobs: " << jobsDone << std::endl;
			}
		}

		if(jobsDone > 0) {
			avg_confs.push_back(std::pair<uint64_t, float>(currentTick, (float)resultsSent / jobsDone));
		}

		if(jobsDone >= (int)m_jobs.size()) {
			break;
		}
	}

	printf("DONE, after tick %lld (jobs: %d, results total: %d)\n", currentTick, jobsDone, resultsSent);
	printf("correctness ratio: %g\n", (float)resultsSent / jobsDone);


	if(nograph) {
		return;
	}

	gp << "plot";

	for(auto plotit = plots.begin(); plotit != plots.end(); ++plotit) {
		auto node_plot = &plotit->second;

		//gp << gp.file1d(node_plot->trust_abs) << "with lines title 'abs_trust_" << node_plot->id << "', ";
		gp << gp.file1d(node_plot->trust) << "with lines title 'Node " << node_plot->id << "', ";
		//std::cout << gp.file1d(node_plot->trust) << "with lines title 'Node " << node_plot->id << "', ";
		//gp << gp.file1d(avg_confs) << "with lines title 'Confirmation count', ";
		//gp << gp.file1d(node_plot->jobs) << "with points title '' lt rgb \"black\", ";
	}

	gp << std::endl;


#ifdef _WIN32
	// For Windows, prompt for a keystroke before the Gnuplot object goes out of scope so that
	// the gnuplot window doesn't get closed.
	std::cout << "Press enter to exit." << std::endl;
	std::cin.get();
#endif
}
